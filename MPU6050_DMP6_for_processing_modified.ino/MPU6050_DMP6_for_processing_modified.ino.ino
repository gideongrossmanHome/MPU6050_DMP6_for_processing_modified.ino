// I2C device class (I2Cdev) demonstration Arduino sketch for MPU6050 class using DMP (MotionApps v2.0)
// 6/21/2012 by Jeff Rowberg <jeff@rowberg.net>
// Updates should (hopefully) always be available at https://github.com/jrowberg/i2cdevlib
//
// Changelog:
//      2013-05-08 - added seamless Fastwire support
//                 - added note about gyro calibration
//      2012-06-21 - added note about Arduino 1.0.1 + Leonardo compatibility error
//      2012-06-20 - improved FIFO overflow handling and simplified read process
//      2012-06-19 - completely rearranged DMP initialization code and simplification
//      2012-06-13 - pull gyro and accel data from FIFO packet instead of reading directly
//      2012-06-09 - fix broken FIFO read sequence and change interrupt detection to RISING
//      2012-06-05 - add gravity-compensated initial reference frame acceleration output
//                 - add 3D math helper file to DMP6 example sketch
//                 - add Euler output and Yaw/Pitch/Roll output formats
//      2012-06-04 - remove accel offset clearing for better results (thanks Sungon Lee)
//      2012-06-01 - fixed gyro sensitivity to be 2000 deg/sec instead of 250
//      2012-05-30 - basic DMP initialization working

/* ============================================
I2Cdev device library code is placed under the MIT license
Copyright (c) 2012 Jeff Rowberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/

// I2Cdev and MPU6050 must be installed as libraries, or else the .cpp/.h files
// for both classes must be in the include path of your project
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#include "MPU6050.h" // not necessary if using MotionApps include file

// Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation
// is used in I2Cdev.h
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

// class default I2C address is 0x68
// specific I2C addresses may be passed as a parameter here
// AD0 low = 0x68 (default for SparkFun breakout and InvenSense evaluation board)
// AD0 high = 0x69
MPU6050 mpu; //commented out by Gideon
//MPU6050 mpu(0x69); // <-- use for AD0 high


/* =========================================================================
   NOTE: In addition to connection 3.3v, GND, SDA, and SCL, this sketch
   depends on the MPU-6050's INT pin being connected to the Arduino's
   external interrupt #0 pin. On the Arduino Uno and Mega 2560, this is
   digital I/O pin 2.
 * ========================================================================= */

/* =========================================================================
   NOTE: Arduino v1.0.1 with the Leonardo board generates a compile error
   when using Serial.write(buf, len). The Teapot output uses this method.
   The solution requires a modification to the Arduino USBAPI.h file, which
   is fortunately simple, but annoying. This will be fixed in the next IDE
   release. For more info, see these links:

   http://arduino.cc/forum/index.php/topic,109987.0.html
   http://code.google.com/p/arduino/issues/detail?id=958
 * ========================================================================= */



// uncomment "OUTPUT_READABLE_QUATERNION" if you want to see the actual
// quaternion components in a [w, x, y, z] format (not best for parsing
// on a remote host such as Processing or something though)
//#define OUTPUT_READABLE_QUATERNION

// uncomment "OUTPUT_READABLE_EULER" if you want to see Euler angles
// (in degrees) calculated from the quaternions coming from the FIFO.
// Note that Euler angles suffer from gimbal lock (for more info, see
// http://en.wikipedia.org/wiki/Gimbal_lock)
//#define OUTPUT_READABLE_EULER

// uncomment "OUTPUT_READABLE_YAWPITCHROLL" if you want to see the yaw/
// pitch/roll angles (in degrees) calculated from the quaternions coming
// from the FIFO. Note this also requires gravity vector calculations.
// Also note that yaw/pitch/roll angles suffer from gimbal lock (for
// more info, see: http://en.wikipedia.org/wiki/Gimbal_lock)
//#define OUTPUT_READABLE_YAWPITCHROLL

// uncomment "OUTPUT_READABLE_REALACCEL" if you want to see acceleration
// components with gravity removed. This acceleration reference frame is
// not compensated for orientation, so +X is always +X according to the
// sensor, just without the effects of gravity. If you want acceleration
// compensated for orientation, us OUTPUT_READABLE_WORLDACCEL instead.
//#define OUTPUT_READABLE_REALACCEL

// uncomment "OUTPUT_READABLE_WORLDACCEL" if you want to see acceleration
// components with gravity removed and adjusted for the world frame of
// reference (yaw is relative to initial orientation, since no magnetometer
// is present in this case). Could be quite handy in some cases.
#define OUTPUT_READABLE_WORLDACCEL

// uncomment "OUTPUT_TEAPOT" if you want output that matches the
// format used for the InvenSense teapot demo
//#define OUTPUT_TEAPOT

//#define OUTPUT_RAW_TRIPPED_ACCEL


#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
bool blinkState = false;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

//conversion constants
#define ADC_PER_G 16384
float M_S_S_PER_G = 9.80665;

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 gyro;          // [x, y, z]
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorInt16 aaWorldPostOffset; // [x, y, z]
VectorFloat aaWorldPostOffsetInMetric;
VectorFloat gravity;    // [x, y, z]            gravity vector
//float euler[3];         // [psi, theta, phi]    Euler angle container
//float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

//int stationary_accel_threshold = 40;
//#define SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY 100
//int16_t dynamic_accel_offset_array[3][SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY];
//VectorInt16 dynamic_accel_offset_array_filling_counter;
//VectorInt16 aaWorldOffsets;
//int32_t dynamic_accel_offset_sum[3] = {0,0,0};

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };
uint8_t accelPacket[18] = { '$', 0x02, 0,0, 0,0 ,0,0, 0,0, 0,0,0,0,0x00, 0x00, '\r', '\n' };
int16_t ax, ay, az,gx, gy, gz; //for testing getMotion6
void ResetAccelOffsetCalculator(int axis);
void ShiftArrayForward(uint16_t arr[], uint16_t array_size);
int32_t AverageArray(int16_t arr[], uint16_t elements_to_average, int axis);
boolean SettlingTimeElapsed();
#define INITIAL_SETTLING_TIME 10000

//moving average i.e. low pass filter
#define SIZE_OF_MOVING_AVERAGE_ARRAY 10
int16_t moving_average_array[3][SIZE_OF_MOVING_AVERAGE_ARRAY];
VectorInt16 aaWorldAfterLowPassFilter;

//stationary detection
#define NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY 10
uint8_t not_moving_count[3] = {0,0,0};
void CheckIfAccelerating(VectorInt16 new_val);
VectorBool is_moving(true,true,true);

//velocity variables
//VectorFloat vWorld; // m/s
//float vWorldPostOffset;
//int stationary_vel_threshold = 40;
//#define SIZE_OF_VEL_DYNAMIC_OFFSET_ARRAY SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY
//float dynamic_vel_offset_array[SIZE_OF_VEL_DYNAMIC_OFFSET_ARRAY];
//int16_t dynamic_vel_offset_array_filling_counter = 0;
//VectorFloat vWorldOffsets;
//int32_t dynamic_vel_offset_sum = 0;
//void ResetVelOffsetCalculator();

// Outputting data
void PrintAccel();
void PrintVelocity();

//time keeping
unsigned long elapsed_time = 0.01; //milliseconds



#define PRINT_DATA_BUFFER_SIZE 100
VectorInt16 accel_print_buffer[PRINT_DATA_BUFFER_SIZE]; // may need to enlarge
unsigned long print_buffer_timer = 0;
unsigned long start_print_buffer_timer = 0;
#define PRINT_BUFFER_DURATION 450 //ms

#define PROCESS_DATA_SM_START_STATE 0
#define PROCESS_DATA_SM_RUNNING_STATE 1
#define PROCESS_DATA_SM_END_STATE 2
int print_buffer_state = PROCESS_DATA_SM_END_STATE;
#define ACCEL_TRIP_VALUE_M_S_S 0.5
#define ACCEL_TRIP_VALUE ACCEL_TRIP_VALUE_M_S_S / M_S_S_PER_G * ADC_PER_G
void ClearVectorInt16Buffer(VectorInt16 buffer[], uint16_t size_of_array);
void UpdateVectorInt16Buffer(VectorInt16 buffer[], VectorInt16 new_val, uint16_t array_size);
void ClearQuaternionBuffer(Quaternion buffer[], uint16_t size_of_array);
void UpdateQuaternionBuffer(Quaternion buffer[], Quaternion new_val, uint16_t array_size);
#define INTERVALS_BUFFER_LENGTH PRINT_DATA_BUFFER_SIZE
unsigned long intervals[INTERVALS_BUFFER_LENGTH];
// ===================e=============================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}



// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {
    // join I2C bus (I2Cdev library doesn't do this automatically)
    #if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
        Wire.begin();
        TWBR = 40; // 400kHz I2C clock (200kHz if CPU is 8MHz)
    #elif I2CDEV_IMPLEMENTATION == I2CDEV_BUILTIN_FASTWIRE
        Fastwire::setup(400, true);
    #endif

    // initialize serial communication
    // (115200 chosen because it is required for Teapot Demo output, but it's
    // really up to you depending on your project)
    Serial.begin(115200);
    while (!Serial); // wait for Leonardo enumeration, others continue immediately

    // NOTE: 8MHz or slower host processors, like the Teensy @ 3.3v or Ardunio
    // Pro Mini running at 3.3v, cannot handle this baud rate reliably due to
    // the baud timing being too misaligned with processor ticks. You must use
    // 38400 or slower in these cases, or use some kind of external separate
    // crystal solution for the UART timer.

    
    // initialize device
    Serial.println(F("Initializing I2C devices..."));
    mpu.initialize();

    // verify connection
    Serial.println(F("Testing device connections..."));
    Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

    // wait for ready
    Serial.println(F("\nSend any character to begin DMP programming and demo: "));
    while (Serial.available() && Serial.read()); // empty buffer
    while (!Serial.available());                 // wait for data
    while (Serial.available() && Serial.read()); // empty buffer again

    // load and configure the DMP
    Serial.println(F("Initializing DMP..."));
    devStatus = mpu.dmpInitialize();

    // supply your own gyro offsets here, scaled for min sensitivity
    mpu.setXGyroOffset(-4);
    mpu.setYGyroOffset(-19);
    mpu.setZGyroOffset(8);
    mpu.setXAccelOffset(-2014);
    mpu.setYAccelOffset(2092);
    mpu.setZAccelOffset(1044);
 
    // make sure it worked (returns 0 if so)
    if (devStatus == 0) {
        // turn on the DMP, now that it's ready
        Serial.println(F("Enabling DMP..."));
        mpu.setDMPEnabled(true);

        // enable Arduino interrupt detection
        Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
        attachInterrupt(0, dmpDataReady, RISING);
        mpuIntStatus = mpu.getIntStatus();

        // set our DMP Ready flag so the main loop() function knows it's okay to use it
        Serial.println(F("DMP ready! Waiting for first interrupt..."));
        dmpReady = true;

        // get expected DMP packet size for later comparison
        packetSize = mpu.dmpGetFIFOPacketSize();
        Serial.print("packet size:\t"); Serial.println(packetSize);
    } else {
        // ERROR!
        // 1 = initial memory load failed
        // 2 = DMP configuration updates failed
        // (if it's going to break, usually the code will be 1)
        Serial.print(F("DMP Initialization failed (code "));
        Serial.print(devStatus);
        Serial.println(F(")"));
    }

    // configure LED for output
    pinMode(LED_PIN, OUTPUT);
    Serial.print("trip value: \t");
    Serial.println(ACCEL_TRIP_VALUE);
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
  delay(15);

  /*
    // if programming failed, don't try to do anything
    if (!dmpReady) return;

    // wait for MPU interrupt or extra packet(s) available
    while (!mpuInterrupt && fifoCount < packetSize) {
        // other program behavior stuff here
        // .
        // .
        // .
        // if you are really paranoid you can frequently test in between other
        // stuff to see if mpuInterrupt is true, and if so, "break;" from the
        // while() loop to immediately process the MPU data
        // .
        // .
        // .
    }

    // reset interrupt flag and get INT_STATUS byte
    mpuInterrupt = false;
    mpuIntStatus = mpu.getIntStatus();

    // get current FIFO count
    fifoCount = mpu.getFIFOCount();

    // check for overflow (this should never happen unless our code is too inefficient)
    if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
        // reset so we can continue cleanly
        mpu.resetFIFO();
        Serial.println(F("FIFO overflow!"));

    // otherwise, check for DMP data ready interrupt (this should happen frequently)
    } else if (mpuIntStatus & 0x02) {
        // wait for correct available data length, should be a VERY short wait
        while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

        // read a packet from FIFO
        mpu.getFIFOBytes(fifoBuffer, packetSize);
        
        // track FIFO count here in case there is > 1 packet available
        // (this lets us immediately read more without waiting for an interrupt)
        fifoCount -= packetSize;

*/

        mpu.getFIFOBytes(fifoBuffer, packetSize);
        #ifdef OUTPUT_READABLE_QUATERNION
            // display quaternion values in easy matrix form: w x y z
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            Serial.print("quat\t");
            Serial.print(q.w);
            Serial.print("\t");
            Serial.print(q.x);
            Serial.print("\t");
            Serial.print(q.y);
            Serial.print("\t");
            Serial.println(q.z);
        #endif

        #ifdef OUTPUT_READABLE_EULER
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetEuler(euler, &q);
            Serial.print("euler\t");
            Serial.print(euler[0] * 180/M_PI);
            Serial.print("\t");
            Serial.print(euler[1] * 180/M_PI);
            Serial.print("\t");
            Serial.println(euler[2] * 180/M_PI);
        #endif

        #ifdef OUTPUT_READABLE_YAWPITCHROLL
            // display Euler angles in degrees
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
            Serial.print("ypr\t");
            Serial.print(ypr[0] * 180/M_PI);
            Serial.print("\t");
            Serial.print(ypr[1] * 180/M_PI);
            Serial.print("\t");
            Serial.println(ypr[2] * 180/M_PI);
        #endif

        #ifdef OUTPUT_READABLE_REALACCEL
            // display real acceleration, adjusted to remove gravity
            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

//            Serial.print("areal\t");
//            Serial.print(ax);
//            Serial.print("\t");
//            Serial.print(ay);
//            Serial.print("\t");
//            Serial.println(az);
              int accelXByte3= (aa.x & 0xFFFFFF00) >> 24;
              int accelXByte2 = (aa.x & 0xFFFF00) >> 16;
              int accelXByte1 = (aa.x & 0xFF00) >> 8;
              int accelXByte0 = aa.x & 0xFF;
              
              int accelYByte3= (aa.y & 0xFFFFFF00) >> 24;
              int accelYByte2 = (aa.y & 0xFFFF00) >> 16;
              int accelYByte1 = (aa.y & 0xFF00) >> 8;
              int accelYByte0 = aa.y & 0xFF;

              int accelZByte3= (aa.z & 0xFFFFFF00) >> 24;
              int accelZByte2 = (aa.z & 0xFFFF00) >> 16;
              int accelZByte1 = (aa.z & 0xFF00) >> 8;
              int accelZByte0 = aa.z & 0xFF;
//            Serial.print("areal\t");
//            Serial.print(aa.x);
//            Serial.print("\t");
//            Serial.print(aa.y);
//            Serial.print("\t");
//            Serial.println(aa.z);
//
//            int accelXByte3= (aa.z & 0xFFFFFF00) >> 24;
//            int accelXByte2 = (aa.z & 0xFFFF00) >> 16;
//            int accelXByte1 = (aa.z & 0xFF00) >> 8;
//            int accelXByte0 = aa.z & 0xFF;


            accelPacket[2] = accelXByte3;
            accelPacket[3] = accelXByte2;
            accelPacket[4] = accelXByte1;
            accelPacket[5] = accelXByte0;

            accelPacket[6] = accelYByte3;
            accelPacket[7] = accelYByte2;
            accelPacket[8] = accelYByte1;
            accelPacket[9] = accelYByte0;
            
            accelPacket[10] = accelZByte3;
            accelPacket[11] = accelZByte2;
            accelPacket[12] = accelZByte1;
            accelPacket[13] = accelZByte0;
            Serial.write(accelPacket, 18);
            accelPacket[18]++;
        #endif

        #ifdef OUTPUT_RAW_TRIPPED_ACCEL
          CalculateTimeSinceLastMeasurement();
          mpu.dmpGetQuaternion(&q, fifoBuffer);
          mpu.dmpGetAccel(&aa, fifoBuffer);
          mpu.dmpGetGravity(&gravity, &q);
          mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
          mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);
          RunPrintBufferSM();
        #endif

        #ifdef OUTPUT_READABLE_WORLDACCEL
            // display initial world-frame acceleration, adjusted to remove gravity
            // and rotated based on known orientation from quaternion
//            mpu.dmpGetQuaternion(&q, fifoBuffer);
            mpu.dmpGetAccel(&aa, fifoBuffer);
            Serial.println(millis());
            /*
//            mpu.dmpGetGyro(&gyro, fifoBuffer);
            gyro.x = ((fifoBuffer[16] << 24) + (fifoBuffer[17] << 16) + (fifoBuffer[18] << 8) + fifoBuffer[19]);
            gyro.y = ((fifoBuffer[20] << 24) + (fifoBuffer[21] << 16) + (fifoBuffer[22] << 8) + fifoBuffer[23]);
            gyro.z = ((fifoBuffer[24] << 24) + (fifoBuffer[25] << 16) + (fifoBuffer[26] << 8) + fifoBuffer[27]);
            mpu.dmpGetGravity(&gravity, &q);
            mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
            mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);
//            Serial.print((String)gyro.x + "\t"+(String)gyro.y + "\t"+(String)gyro.z + "\t");
//            Serial.print("g:");
            Serial.print(gyro.x); Serial.print("\t");
            Serial.print(gyro.y); Serial.print("\t");
            Serial.print(gyro.z); Serial.print("\t");
//            Serial.print("aa:");
//            Serial.print((String)aa.x + "\t"+(String)aa.y + "\t"+(String)aa.z + "\t");
            Serial.print(aa.x); Serial.print("\t");
            Serial.print(aa.y); Serial.print("\t");
            Serial.print(aa.z); Serial.print("\t");
//            Serial.print("q:");
//            Serial.print((String)q.w + "\t"+(String)q.x + "\t"+(String)q.y + "\t"+(String)q.z + "\t");
            Serial.print(q.w); Serial.print("\t");
            Serial.print(q.x); Serial.print("\t");
            Serial.print(q.y); Serial.print("\t");
            Serial.print(q.z); Serial.print("\t");
//            Serial.print("grav:");
            Serial.print(gravity.x); Serial.print("\t");
            Serial.print(gravity.y); Serial.print("\t");
            Serial.print(gravity.z); Serial.print("\t");
//            Serial.print("aaReal:");
            Serial.print(aaReal.x); Serial.print("\t");
            Serial.print(aaReal.y); Serial.print("\t");
            Serial.print(aaReal.z); Serial.print("\t");    
//            Serial.print("aaWorld:");      
            Serial.print(aaWorld.x); Serial.print("\t");
            Serial.print(aaWorld.y); Serial.print("\t");
            Serial.print(aaWorld.z); Serial.print("\t");
            Serial.println("");
              
              int accelXByte3= (aaWorld.x & 0xFFFFFF00) >> 24;
              int accelXByte2 = (aaWorld.x & 0xFFFF00) >> 16;
              int accelXByte1 = (aaWorld.x & 0xFF00) >> 8;
              int accelXByte0 = aaWorld.x & 0xFF;
              
              int accelYByte3= (aaWorld.y & 0xFFFFFF00) >> 24;
              int accelYByte2 = (aaWorld.y & 0xFFFF00) >> 16;
              int accelYByte1 = (aaWorld.y & 0xFF00) >> 8;
              int accelYByte0 = aaWorld.y & 0xFF;

              int accelZByte3= (aaWorld.z & 0xFFFFFF00) >> 24;
              int accelZByte2 = (aaWorld.z & 0xFFFF00) >> 16;
              int accelZByte1 = (aaWorld.z & 0xFF00) >> 8;
              int accelZByte0 = aaWorld.z & 0xFF;
//            Serial.print("areal\t");
//            Serial.print(aa.x);
//            Serial.print("\t");
//            Serial.print(aa.y);
//            Serial.print("\t");
//            Serial.println(aa.z);
//
//            int accelXByte3= (aa.z & 0xFFFFFF00) >> 24;
//            int accelXByte2 = (aa.z & 0xFFFF00) >> 16;
//            int accelXByte1 = (aa.z & 0xFF00) >> 8;
//            int accelXByte0 = aa.z & 0xFF;


//            accelPacket[2] = accelXByte3;
//            accelPacket[3] = accelXByte2;
//            accelPacket[4] = accelXByte1;
//            accelPacket[5] = accelXByte0;
//
//            accelPacket[6] = accelYByte3;
//            accelPacket[7] = accelYByte2;
//            accelPacket[8] = accelYByte1;
//            accelPacket[9] = accelYByte0;
//            
//            accelPacket[10] = accelZByte3;
//            accelPacket[11] = accelZByte2;
//            accelPacket[12] = accelZByte1;
//            accelPacket[13] = accelZByte0;
//            Serial.write(accelPacket, 18);
            accelPacket[18]++;
            
//            Serial.print("aworld\t");
//            Serial.print(aaWorld.x);
//            Serial.print("\t");
//            Serial.print(aaWorld.y);
//            Serial.print("\t");
//            Serial.println(aaWorld.z);
//            if (SettlingTimeElapsed())
//            {
//              CalculateTimeSinceLastMeasurement();
//              GetLowPassFilteredValue(aaWorld);
//              SendAccelDataToOffsetFilter(aaWorldAfterLowPassFilter);
//              ConvertAccelFromADCToMetric();
//              UpdateVelocity();
//              PrintResults();
//            }
//            Serial.write(accelPacket, 14);
*/
        #endif
    
        #ifdef OUTPUT_TEAPOT
            // display quaternion values in InvenSense Teapot demo format:
            teapotPacket[2] = fifoBuffer[0];
            teapotPacket[3] = fifoBuffer[1];`
            teapotPacket[4] = fifoBuffer[4];
            teapotPacket[5] = fifoBuffer[5];
            teapotPacket[6] = fifoBuffer[8];
            teapotPacket[7] = fifoBuffer[9];
            teapotPacket[8] = fifoBuffer[12];
            teapotPacket[9] = fifoBuffer[13];
            Serial.write(teapotPacket, 14);
            teapotPacket[11]++; // packetCount, loops at 0xFF on purpose
        #endif

        // blink LED to indicate activity
        blinkState = !blinkState;
        digitalWrite(LED_PIN, blinkState);
    
}
//
//void SendAccelDataToOffsetFilter(VectorInt16 world_accel_before_offset)
//{
//  CheckIfAccelerating(world_accel_before_offset);
//  if (!is_moving.x)
//  {
//    aaWorldPostOffset.x = 0;
//    RunDynamicOffsetCalculator(world_accel_before_offset.x,0);
//  }
//  else //is still moving
//  {
//      aaWorldPostOffset.x = world_accel_before_offset.x - aaWorldOffsets.x;
//      ResetAccelOffsetCalculator(0);
//  }
//    if (!is_moving.y)
//  {
//    aaWorldPostOffset.y = 0;
//    RunDynamicOffsetCalculator(world_accel_before_offset.y,1);
//  }
//  else //is still moving
//  {
//      aaWorldPostOffset.y = world_accel_before_offset.y - aaWorldOffsets.y;
//      ResetAccelOffsetCalculator(1);
//  }
//    if (!is_moving.z)
//  {
//    aaWorldPostOffset.z = 0;
//    RunDynamicOffsetCalculator(world_accel_before_offset.z,2);
//  }
//  else //is still moving
//  {
//      aaWorldPostOffset.z = world_accel_before_offset.z - aaWorldOffsets.z;
//      ResetAccelOffsetCalculator(2);
//  }
//}

//void RunDynamicOffsetCalculator(int16_t new_val, int axis)
//{
//  ShiftArrayForward(dynamic_accel_offset_array[axis], SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY);
//  dynamic_accel_offset_array[axis][0] = new_val;
//
//  if (axis == 0)
//  {
//    if (dynamic_accel_offset_array_filling_counter.x < SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      dynamic_accel_offset_array_filling_counter.x ++;
//      aaWorldOffsets.x = AverageArray(dynamic_accel_offset_array[0], dynamic_accel_offset_array_filling_counter.x, 0);
//    }
//    else if (dynamic_accel_offset_array_filling_counter.x == SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      aaWorldOffsets.x = AverageArray(dynamic_accel_offset_array[0], SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY, 0);
//    }
//  }
//
//  else if (axis == 1)
//  {
//    if (dynamic_accel_offset_array_filling_counter.y < SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      dynamic_accel_offset_array_filling_counter.y ++;
//      aaWorldOffsets.y = AverageArray(dynamic_accel_offset_array[1], dynamic_accel_offset_array_filling_counter.y, 1);
//    }
//    else if (dynamic_accel_offset_array_filling_counter.y == SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      aaWorldOffsets.y = AverageArray(dynamic_accel_offset_array[1], SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY, 1);
//    }
//  }
//
//  if (axis == 2)
//  {
//    if (dynamic_accel_offset_array_filling_counter.z < SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      dynamic_accel_offset_array_filling_counter.z ++;
//      aaWorldOffsets.z = AverageArray(dynamic_accel_offset_array[2], dynamic_accel_offset_array_filling_counter.z, 2);
//    }
//    else if (dynamic_accel_offset_array_filling_counter.z == SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY)
//    {
//      aaWorldOffsets.z = AverageArray(dynamic_accel_offset_array[2], SIZE_OF_ACCEL_DYNAMIC_OFFSET_ARRAY, 2);
//    }
//  }
//}
//
//void ResetAccelOffsetCalculator(int axis)
//{
//  if (axis == 0)
//  {
//  dynamic_accel_offset_array_filling_counter.x = 0;
//  dynamic_accel_offset_sum[0] = 0;
//  }
//  else if (axis == 1)
//  {
//  dynamic_accel_offset_array_filling_counter.y = 0;
//  dynamic_accel_offset_sum[1] = 0;
//  }
//  else if (axis == 2)
//  {
//  dynamic_accel_offset_array_filling_counter.z = 0;
//  dynamic_accel_offset_sum[2] = 0;
//  }
//}

void ShiftArrayForward(int16_t arr[], uint16_t array_size)
{
  for (int i = array_size - 1; i > 0; i--)
  {
    arr[i] = arr[i - 1];
  }
}

//int32_t AverageArray(int16_t arr[], uint16_t elements_to_average, int axis)
//{
//    for(int i = 0; i < elements_to_average; i++)
//    {
//        dynamic_accel_offset_sum[axis] += arr[i];
//    }
//    int16_t offset = round(dynamic_accel_offset_sum[axis] * 1.0 / elements_to_average);
//    dynamic_accel_offset_sum[axis] = 0;
//    return offset;
//}

boolean SettlingTimeElapsed()
{
  if (millis() > INITIAL_SETTLING_TIME)
  {
    return true;
  }
  return false;
}


/*
void UpdateMovingAverageFilter(int16_t new_val, int axis)
{
    ShiftArrayForward(moving_average_array[axis], SIZE_OF_MOVING_AVERAGE_ARRAY);
    moving_average_array[axis][0] = new_val;
}

void GetLowPassFilteredValue(VectorInt16 new_val)
{
    UpdateMovingAverageFilter(new_val.x,0);
    aaWorldAfterLowPassFilter.x = AverageArray(moving_average_array[0], SIZE_OF_MOVING_AVERAGE_ARRAY, 0);

    UpdateMovingAverageFilter(new_val.y,1);
    aaWorldAfterLowPassFilter.y = AverageArray(moving_average_array[1], SIZE_OF_MOVING_AVERAGE_ARRAY, 1);
    
    UpdateMovingAverageFilter(new_val.z,2);
    aaWorldAfterLowPassFilter.z = AverageArray(moving_average_array[2], SIZE_OF_MOVING_AVERAGE_ARRAY, 2);
}



void CheckIfAccelerating(VectorInt16 new_val)
{
  if(abs(new_val.x) < stationary_accel_threshold)
  {
    if (not_moving_count[0] < NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
    {
      not_moving_count[0] ++;
    }
  }
  else if (abs(new_val.x) >= stationary_accel_threshold)
  {
    not_moving_count[0] = 0;
  }
  if (not_moving_count[0] == NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
  {
      is_moving.x = false;
  }
  else
  {
    is_moving.x = true;
  }

  if(abs(new_val.y) < stationary_accel_threshold)
  {
    if (not_moving_count[1] < NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
    {
      not_moving_count[1] ++;
    }
  }
  else if (abs(new_val.y) >= stationary_accel_threshold)
  {
    not_moving_count[1] = 0;
  }
  if (not_moving_count[1] == NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
  {
      is_moving.y = false;
  }
  else
  {
    is_moving.y = true;
  }

  if(abs(new_val.z) < stationary_accel_threshold)
  {
    if (not_moving_count[2] < NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
    {
      not_moving_count[2] ++;
    }
  }
  else if (abs(new_val.z) >= stationary_accel_threshold)
  {
    not_moving_count[2] = 0;
  }
  if (not_moving_count[2] == NUMBER_OF_LOW_READINGS_CONSIDERED_STATIONARY)
  {
      is_moving.z = false;
  }
  else
  {
    is_moving.z = true;
  }
}


void ConvertAccelFromADCToMetric()
{
  aaWorldPostOffsetInMetric.x  = (aaWorldPostOffset.x * 1.0 / ADC_PER_G * M_S_S_PER_G);
  aaWorldPostOffsetInMetric.y  = (aaWorldPostOffset.y * 1.0 / ADC_PER_G * M_S_S_PER_G);
  aaWorldPostOffsetInMetric.z  = (aaWorldPostOffset.z * 1.0 / ADC_PER_G * M_S_S_PER_G);
}
void UpdateVelocity()
{
  if (is_moving.x)
  {
    vWorld.x += aaWorldPostOffsetInMetric.x * elapsed_time / 100.0;
  }
  else
  {
    vWorld.x = 0;
  }
  
  if (is_moving.y)
  {
    vWorld.y += aaWorldPostOffsetInMetric.y * elapsed_time / 100.0;
  }
  else
  {
    vWorld.y = 0;
  }
  
  if (is_moving.z)
  {
    vWorld.z += aaWorldPostOffsetInMetric.z * elapsed_time / 100.0;
  }
  else
  {
    vWorld.z = 0;
  }  
}

void PrintVelocity()
{
  Serial.print(vWorld.x);
  Serial.print(",");
  Serial.print(vWorld.y);
  Serial.print(",");
  Serial.print(vWorld.z);
}

void PrintAccel()
{
//    Serial.print(aaWorld.x);
//    Serial.print(",");
//    Serial.print(aaWorldAfterLowPassFilter.x);
//    Serial.print(",");
//    Serial.print(aaWorldPostOffset.x);
//    Serial.print(",");

    Serial.print(aaWorldPostOffsetInMetric.x);
    Serial.print(",");
    Serial.print(aaWorldPostOffsetInMetric.y);
    Serial.print(",");
    Serial.print(aaWorldPostOffsetInMetric.z);
    Serial.print(",");
}

void PrintIsAccelerating()
{
  Serial.print(is_moving.x);
  Serial.print(",");
  Serial.print(is_moving.y);
  Serial.print(",");
  Serial.print(is_moving.z);
  Serial.print(",");
}

*/
void CalculateTimeSinceLastMeasurement() // in milliseconds
{
  static unsigned long previous_time = millis();
  unsigned long current_time = millis();
  elapsed_time = current_time - previous_time;
  previous_time = current_time;
}

/*
void PrintElapsedTime()
{
  Serial.print(elapsed_time);
}

void PrintResults()
{
//  PrintElapsedTime();
//  Serial.print(",");
  PrintAccel();
  PrintVelocity();
//  PrintIsAccelerating();
  Serial.print("\r\n");
}

*/

void RunPrintBufferSM()
{
  switch(print_buffer_state)
  {
    case PROCESS_DATA_SM_START_STATE :
      start_print_buffer_timer = millis();
      ClearVectorInt16Buffer(accel_print_buffer, PRINT_DATA_BUFFER_SIZE);
      UpdateVectorInt16Buffer(accel_print_buffer, aaWorld, PRINT_DATA_BUFFER_SIZE);
      print_buffer_state = PROCESS_DATA_SM_RUNNING_STATE;
    break;
    
    case PROCESS_DATA_SM_RUNNING_STATE :
      UpdateVectorInt16Buffer(accel_print_buffer, aaWorld, PRINT_DATA_BUFFER_SIZE);
      print_buffer_timer = millis();
      UpdateUnsignedLongBuffer(intervals, elapsed_time);
      if (print_buffer_timer > start_print_buffer_timer + PRINT_BUFFER_DURATION)
      {
        ProcessAccelBuffer(accel_print_buffer, PRINT_DATA_BUFFER_SIZE);
//        PrintIntervals(intervals, INTERVALS_BUFFER_LENGTH);
        print_buffer_state = PROCESS_DATA_SM_END_STATE ;
      }
    break;
    
    case PROCESS_DATA_SM_END_STATE:
//        Serial.print(aaWorld.x); Serial.print(",");
//        Serial.print(aaWorld.y); Serial.print(",");
//        Serial.print(aaWorld.z); Serial.print(",");
        if (abs(aaWorld.x) > ACCEL_TRIP_VALUE | abs(aaWorld.y) > ACCEL_TRIP_VALUE  | abs(aaWorld.z) > ACCEL_TRIP_VALUE )
        {
//          Serial.println("tripped\r\n");
          print_buffer_state = PROCESS_DATA_SM_START_STATE;
        }
//        else { Serial.println("nope\r\n");}
    /*Process buffer*/
      break;
  }
}

void ProcessAccelBuffer(VectorInt16 accel_buffer[], uint16_t array_size)
{
//  PrintVectorInt16Buffer(raw_accel_buffer, PRINT_DATA_BUFFER_SIZE);
  for (int i = array_size - 1; i > -1; i--)
  {
    Serial.print(intervals[i]); Serial.print(",");
    Serial.print(accel_buffer[i].x); Serial.print(",");
    Serial.print(accel_buffer[i].y); Serial.print(",");
    Serial.print(accel_buffer[i].z); Serial.print(",");
    Serial.print("\r\n");
  }
}
void UpdateVectorInt16Buffer(VectorInt16 buffer[], VectorInt16 new_val, uint16_t array_size)
{
  ShiftVectorInt16ArrayForward(buffer, array_size);
  buffer[0] = new_val;
}

void UpdateQuaternionBuffer(Quaternion buffer[], Quaternion new_val, uint16_t array_size)
{
  ShiftQuaternionArrayForward(buffer, array_size);
  buffer[0] = new_val;
}

void UpdateUnsignedLongBuffer(unsigned long buffer[], unsigned long new_val)
{
  ShiftUnsignedLongArrayForward(buffer, INTERVALS_BUFFER_LENGTH);
  buffer[0] = new_val;
}

void ShiftUnsignedLongArrayForward(unsigned long arr[], int array_size)
{
  for (int i = array_size - 1; i > 0; i--)
  {
    arr[i] = arr[i - 1];
  }
}

void ShiftVectorInt16ArrayForward(VectorInt16 arr[], uint16_t array_size)
{
  for (int i = array_size - 1; i > 0; i--)
  {
    arr[i].x = arr[i - 1].x;
    arr[i].y = arr[i - 1].y;
    arr[i].z = arr[i - 1].z;
  }
}

void ShiftQuaternionArrayForward(Quaternion arr[], uint16_t array_size)
{
  for (int i = array_size - 1; i > 0; i--)
  {
    arr[i].w = arr[i - 1].w;
    arr[i].x = arr[i - 1].x;
    arr[i].y = arr[i - 1].y;
    arr[i].z = arr[i - 1].z;
  }
}

void PrintVectorInt16Buffer(VectorInt16 buffer[], uint16_t size_of_array)
{
    for (int i = 0; i < size_of_array; i++)
    {
        Serial.print(buffer[i].x);
        Serial.print(",");
        Serial.print(buffer[i].y);
        Serial.print(",");
        Serial.print(buffer[i].z);
        Serial.print("\r\n");
    }
}

void PrintIntervals(unsigned long buffer[], int array_size)
{
  Serial.print("in PrintIntervals\r\n");
  for (int i = 0; i < INTERVALS_BUFFER_LENGTH; i++)
  {
    Serial.print("t: ");
    Serial.print(intervals[i]);
    Serial.println("\r\n");
  }
}
void ClearVectorInt16Buffer(VectorInt16 buffer[], uint16_t size_of_array)
{
    for (int i = 0; i < size_of_array; i++)
    {
        buffer[i].x = 0;
        buffer[i].y = 0;
        buffer[i].z = 0;
    }
}

void ClearQuaternionBuffer(Quaternion buffer[], uint16_t size_of_array)
{
    for (int i = 0; i < size_of_array; i++)
    {
        buffer[i].w = 0;
        buffer[i].x = 0;
        buffer[i].y = 0;
        buffer[i].z = 0;
    }
}
