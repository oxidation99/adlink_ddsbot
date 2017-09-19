/*
Copyright 2017 ADLINK Technology Inc.
Developer: Chester, Tseng (for pySerial & encoder)
           HaoChih, LIN (for controller)
Email: chester.tseng@adlinktech.com
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <MsTimer2.h>

// Pin define
static const int out1_pin = 5;
static const int out2_pin = 6;
static const int out3_pin = 9;
static const int out4_pin = 10;
static const int decoder_pin_1 = 3;
static const int decoder_pin_2 = 2;

// Timer internal
static const int timer_period = 50; // ms 
static const float timer_hz = 1000.0/( (float)timer_period ); // hz
static const float timer_dt = timer_period/1000.0; // sec
static const int filter_size = 10;
static const int counter_protect = 200;

// encoder
volatile int decoder_pin_1_counter = 0;
volatile int decoder_pin_2_counter = 0;
volatile int decoder_pin_1_delay_counter = 0;
volatile int decoder_pin_2_delay_counter = 0;
volatile int encoder_res = 18; // 20 pulse in a circle --> 360/20 = 18(deg/pulse)
volatile int filter_pin1[filter_size] = {}; // all zeros
volatile int filter_pin2[filter_size] = {}; // all zeros
volatile float total_pin1 = 0.0;
volatile float total_pin2 = 0.0;
volatile int filterId = 0;

// controller
volatile float WL_ref = 0.0; //reference speed for left wheel (deg/sec) 
volatile float WR_ref = 0.0;
static const float Kp_L = 0.25; //0.11 //without loading
static const float Kp_R = 0.25;
static const float Ki_L = 0.1;
static const float Ki_R = 0.1;
static const float Kd_L = 0.005;
static const float Kd_R = 0.005;
volatile float integralL = 0.0;
volatile float integralR = 0.0;
volatile float prev_error_L = 0.0;
volatile float prev_error_R = 0.0;
static const float integral_dec = 0.995; // for position control, it should be 1.0
static const float integral_max = 400;
static const float integral_min = -400;

void setup() {
  // Set 4 pwm channel pin to output
  pinMode(out1_pin, OUTPUT);
  pinMode(out2_pin, OUTPUT);
  pinMode(out3_pin, OUTPUT);
  pinMode(out4_pin, OUTPUT);

  // Set 4 PWM channel pin to duty cycle: 0
  analogWrite(out1_pin, 0);
  analogWrite(out2_pin, 0);
  analogWrite(out3_pin, 0);
  analogWrite(out4_pin, 0);

  // Start serial port
  Serial.begin(115200);
  while (!Serial) {
  }
  Serial.setTimeout(20);
  
 // Set 2 external interrupt pin to input and attach it's ISR
  pinMode(decoder_pin_1, INPUT);
  pinMode(decoder_pin_2, INPUT);
  attachInterrupt(digitalPinToInterrupt(decoder_pin_1), decoder_1_isr, FALLING);
  attachInterrupt(digitalPinToInterrupt(decoder_pin_2), decoder_2_isr, FALLING);

  // Start timer for motor control & reporting wheel speed 
  MsTimer2::set(timer_period, controller_repoter_isr); 
  MsTimer2::start();
}

void loop() {
    if (Serial.available()) {
      String commad_string = Serial.readString();
      float out_1 = getValue(commad_string, ',', 0).toFloat(); //getValue func should be removed (TBD) 
      float out_2 = getValue(commad_string, ',', 1).toFloat();
      float out_3 = getValue(commad_string, ',', 2).toFloat();
      float out_4 = getValue(commad_string, ',', 3).toFloat();
      
      WL_ref =  out_1 - out_2; // deg/sec, pos: forward, neg: backward
      WR_ref =  out_3 - out_4; // deg/sec
    }
}

void decoder_1_isr() {
  decoder_pin_1_counter += 1;
}

void decoder_2_isr() {
  decoder_pin_2_counter += 1;
}

// convert required deg/s to pwm command (based on statistical)
float feedforward(float value) {
  //return 0.1335 * value + 7.07; //withous laoding
    return 0.5 * value;
}

void controller_repoter_isr() {
  // copy current counter
  int pin1_counter = decoder_pin_1_counter;
  int pin2_counter = decoder_pin_2_counter;
  
  // average filter of encoder data
  total_pin1 = total_pin1 - (float)filter_pin1[filterId];
  total_pin2 = total_pin2 - (float)filter_pin2[filterId];
  filter_pin1[filterId] = pin1_counter;
  filter_pin2[filterId] = pin2_counter;
  total_pin1 = total_pin1 + (float)filter_pin1[filterId];
  total_pin2 = total_pin2 + (float)filter_pin2[filterId];
  filterId = filterId + 1;
  if(filterId >= filter_size)
    filterId = 0;
  
  float average_counter_pin1 = total_pin1/((float)filter_size);
  float average_counter_pin2 = total_pin2/((float)filter_size);
  //Serial.println(average_counter_pin2,4); //debug

  // controller (feedforward + feedback)
  float cmd = 0.0;
  float error = 0.0;
  if(WL_ref > 0.0)
  {
    error = WL_ref - average_counter_pin1*(float)(encoder_res)*timer_hz; // deg/sec
    integralL = integralL*integral_dec + error*timer_dt;
    if(integralL > integral_max)
      integralL = integral_max;
    if(integralL < integral_min)
      integralL = integral_min;
    float der_error = (error - prev_error_L)/timer_dt;  
    prev_error_L = error;
    cmd = feedforward(WL_ref) + error*Kp_L + integralL*Ki_L + der_error*Kd_L; // controller
    
    if(cmd>=0.0)
    {
      analogWrite(out1_pin, (int)cmd);
      analogWrite(out2_pin, 0);
    }
    else
    {
      analogWrite(out1_pin, 0);
      analogWrite(out2_pin, -(int)cmd);
    }
  }
  else if(WL_ref < 0.0) // the encoder has no capability of distinguishing the cw/ccw rotation
  {
    error = -WL_ref - average_counter_pin1*(float)(encoder_res)*timer_hz; // deg/sec
    integralL = integralL*integral_dec + error*timer_dt;
    if(integralL > integral_max)
      integralL = integral_max;
    if(integralL < integral_min)
      integralL = integral_min;
    float der_error = (error - prev_error_L)/timer_dt;  
    prev_error_L = error;
    cmd = feedforward(-WL_ref) + error*Kp_L + integralL*Ki_L + der_error*Kd_L; // controller
    
    if(cmd>=0.0)
    {
      analogWrite(out2_pin, (int)cmd);
      analogWrite(out1_pin, 0);
    }
    else
    {
      analogWrite(out2_pin, 0);
      analogWrite(out1_pin, -(int)cmd);
    }
  }
  else // == 0
  {
    integralL = 0.0;
    analogWrite(out1_pin, 0);
    analogWrite(out2_pin, 0);
  }

  if(WR_ref > 0.0)
  {
    error = WR_ref - average_counter_pin2*(float)(encoder_res)*timer_hz; // deg/sec
    integralR = integralR*integral_dec + error*timer_dt;
    if(integralR > integral_max)
      integralR = integral_max;
    if(integralR < integral_min)
      integralR = integral_min;
    float der_error = (error - prev_error_R)/timer_dt;  
    prev_error_R = error;
    cmd = feedforward(WR_ref) + error*Kp_R + integralR*Ki_R + der_error*Kd_R; // controller
    if(cmd>=0.0)
    {
      analogWrite(out3_pin, (int)cmd);
      analogWrite(out4_pin, 0);
    }
    else
    {
      analogWrite(out3_pin, 0);
      analogWrite(out4_pin, -(int)cmd);
    }
  }
  else if(WR_ref < 0.0)// the encoder has no capability of distinguishing the cw/ccw rotation
  {
    error = -WR_ref - average_counter_pin2*(float)(encoder_res)*timer_hz; // deg/sec
    integralR = integralR*integral_dec + error*timer_dt;
    if(integralR > integral_max)
      integralR = integral_max;
    if(integralR < integral_min)
      integralR = integral_min;
    float der_error = (error - prev_error_R)/timer_dt;  
    prev_error_R = error;
    cmd = feedforward(-WR_ref) + error*Kp_R + integralR*Ki_R + der_error*Kd_R; // controller
    if(cmd>=0.0)
    {
      analogWrite(out4_pin, (int)cmd);
      analogWrite(out3_pin, 0);
    }
    else
    {
      analogWrite(out4_pin, 0);
      analogWrite(out3_pin, -(int)cmd);
    }
  }
  else // == 0
  {
    integralR = 0.0;
    analogWrite(out3_pin, 0);
    analogWrite(out4_pin, 0);
  }

  
  
  
  // This is a workaround to prevent from motor idle when speed  = 0;
  if (decoder_pin_1_delay_counter > counter_protect ) {
    WR_ref = 0.0;
    analogWrite(out3_pin, 0);
    analogWrite(out4_pin, 0);
    decoder_pin_1_delay_counter = 0;
  }
  
  if (decoder_pin_2_delay_counter > counter_protect ) {
    WL_ref = 0.0;
    analogWrite(out1_pin, 0);
    analogWrite(out2_pin, 0);
    decoder_pin_2_delay_counter = 0;
  }

  // report encoder info (deg/sec)
  char str[16];
  sprintf(str, "%d,%d\r\n", (int)(average_counter_pin1*(float)encoder_res*timer_hz)
                          , (int)(average_counter_pin2*(float)encoder_res*timer_hz) ); // deg/sec
  Serial.print(str);

  if (decoder_pin_1_counter == 0)
    decoder_pin_1_delay_counter += 1;
  else
    decoder_pin_1_delay_counter = 0;
  if (decoder_pin_2_counter == 0)
    decoder_pin_2_delay_counter += 1;
  else
    decoder_pin_2_delay_counter = 0;
    
  decoder_pin_1_counter = 0;
  decoder_pin_2_counter = 0;
}

String getValue(String data, char separator, int index)
{
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length()-1;

  for(int i=0; i<=maxIndex && found<=index; i++){
    if(data.charAt(i)==separator || i==maxIndex){
        found++;
        strIndex[0] = strIndex[1]+1;
        strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }

  return found>index ? data.substring(strIndex[0], strIndex[1]) : "";
}


