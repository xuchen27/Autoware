/*
 *  Copyright (c) 2017, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cmath>
#include <thread>

#include <ros/ros.h>
#include <geometry_msgs/TwistStamped.h>
#include <sensor_msgs/Joy.h>

#include "g30esli_interface_util.h"
#include "can_utils/cansend.h"
#include "can_utils/cansend_util.h"
#include "can_utils/ymc_can.h"

namespace
{
// ros param
double g_wheel_base;
int g_mode;
std::string g_device;
int g_loop_rate;
int g_stop_time_sec;

// ros publisher
ros::Publisher g_current_twist_pub;

// variables
uint16_t g_target_velocity_ui16;
uint16_t g_manual_target_velocity_ui16;
int16_t g_steering_angle_deg_i16;
int16_t g_manual_steering_angle_deg_i16;
double g_current_vel_kmph = 0.0;
bool g_terminate_thread = false;
bool g_automode = false;
unsigned char g_shift = 0;
unsigned char g_brake = 0;

// cansend tool
mycansend::CanSender g_cansender;

void twist_cmd_callback(const geometry_msgs::TwistStampedConstPtr &msg)
{
  double target_velocity = msg->twist.linear.x * 3.6; // [m/s] -> [km/h]
  double target_steering_angle_deg = ymc::computeTargetSteeringAngleDegree(msg->twist.angular.z, msg->twist.linear.x, g_wheel_base);
  target_steering_angle_deg += 8.0;
  // factor
  target_velocity           *= 10.0;
  target_steering_angle_deg *= 10.0;

  g_target_velocity_ui16    = target_velocity;
  g_steering_angle_deg_i16  = target_steering_angle_deg * -1.0;
}

void current_vel_callback(const geometry_msgs::TwistStampedConstPtr &msg)
{
  g_current_vel_kmph = msg->twist.linear.x * 3.6;
}

void current_joy_callback(const sensor_msgs::JoyConstPtr &msg)
{
  if (msg->buttons[0] == 1 || msg->axes[1] != 0.0 || msg->axes[2] != 0.0)
  {
    g_automode = false;
  }

  double target_velocity = 0.0;
  if (msg->buttons[1] == 1)
  {
    double r2 = (-msg->axes[4]+1.0)/2.0;  // R2
    target_velocity = 16.0 * r2 + 3.0;
  }

  double l2 = (-msg->axes[3]+1.0)/2.0;  // L2
  double steering_angle_ratio_deg = 20.0 + 17.0 * l2;
  double target_steering_angle_deg = steering_angle_ratio_deg * msg->axes[0]; // L horizontal
  target_steering_angle_deg += 8.0;

  g_brake = 0;
  if (msg->buttons[0] == 1) // square
    g_brake = 1;
  else if (msg->buttons[2] == 1)  //  circle
    g_brake = 2;
  else if (msg->buttons[3] == 1)  //  triangle
    g_brake = 3;

  g_shift = msg->buttons[5];  // R1

  // factor
  target_velocity           *= 10.0;
  target_steering_angle_deg *= 10.0;

  g_manual_target_velocity_ui16    = target_velocity;
  g_manual_steering_angle_deg_i16  = target_steering_angle_deg * -1.0;

  if (msg->buttons[12] == 1)
  {
    g_automode = true;
    g_shift = 0;
  }
}

// receive input from keyboard
// change the mode to manual mode or auto drive mode
void changeMode()
{
  while (!g_terminate_thread)
  {
    if (ymc::kbhit())
    {
      char c = getchar();

      if (c == ' ')
        g_mode = 3;

      if (c == 's')
        g_mode = 8;
    }
    usleep(20000); // sleep 20msec
  }
}

// read can data from the vehicle
void readCanData(FILE* fp)
{
  char buf[64];
  while (fgets(buf, sizeof(buf), fp) != NULL && !g_terminate_thread)
  {
    std::string raw_data = std::string(buf);
    // std::cout << "received data: " << raw_data << std::endl;

    // check if the data is valid
    if (raw_data.size() > 0)
    {
      // split data to strings
      // data format is like this
      // can0  200   [8]  08 00 00 00 01 00 01 29
      std::vector<std::string> parsed_data = ymc::splitString(raw_data);

      // delete first 3 elements
      std::vector<std::string> data = parsed_data;
      data.erase(data.begin(), data.begin() + 3);

      int id = std::stoi(parsed_data.at(1), nullptr, 10);
      double _current_vel_mps = ymc::translateCanData(id, data, &g_mode);
      if(_current_vel_mps != RET_NO_PUBLISH )
      {
	      geometry_msgs::TwistStamped ts;
        ts.header.frame_id = "base_link";
        ts.header.stamp = ros::Time::now();
	      ts.twist.linear.x = _current_vel_mps;
	      g_current_twist_pub.publish(ts);
      }

    }

  }
}

} // namespace

int main(int argc, char *argv[])
{
  // ROS initialization
  ros::init(argc, argv, "g30esli_interface");
  ros::NodeHandle n;
  ros::NodeHandle private_nh("~");

  private_nh.param<double>("wheel_base", g_wheel_base, 2.4);
  private_nh.param<int>("mode", g_mode, 8);
  private_nh.param<std::string>("device", g_device, "can0");
  private_nh.param<int>("loop_rate", g_loop_rate, 100);
  private_nh.param<int>("stop_time_sec", g_stop_time_sec, 1);

  // init cansend tool
  g_cansender.init(g_device);

  // subscriber
  ros::Subscriber twist_cmd_sub = n.subscribe<geometry_msgs::TwistStamped>("twist_cmd", 1, twist_cmd_callback);
  ros::Subscriber current_vel_sub = n.subscribe<geometry_msgs::TwistStamped>("current_velocity", 1, current_vel_callback);
  ros::Subscriber current_joy_sub = n.subscribe<sensor_msgs::Joy>("joy", 1, current_joy_callback);

  // publisher
  g_current_twist_pub = n.advertise<geometry_msgs::TwistStamped>("ymc_current_twist", 10);

  // read can data from candump
  FILE *fp = popen("candump can0", "r");

  // create threads
  std::thread t1(changeMode);
  std::thread t2(readCanData, fp);

  ros::Rate loop_rate = g_loop_rate;
  bool stopping_flag = true;
  while (ros::ok())
  {
    // data
    unsigned char mode              = static_cast<unsigned char>(g_mode);
    unsigned char shift             = g_shift;
;
    uint16_t target_velocity_ui16   = (g_automode) ? g_target_velocity_ui16 : g_manual_target_velocity_ui16;
    int16_t steering_angle_deg_i16  = (g_automode) ? g_steering_angle_deg_i16 : g_manual_steering_angle_deg_i16;
    static unsigned char brake      = g_brake;
    static unsigned char heart_beat = 0;

    // // change to STOP MODE...
    // if (target_velocity_ui16 == 0 || g_mode == 3)
    //   stopping_flag = true;
    //
    // // STOPPING
    // static int stop_count = 0;
    // if (stopping_flag && g_mode == 8)
    // {
    //   // TODO: change steering angle according to current velocity ?
    //   target_velocity_ui16 = 0;
    //   brake = 1;
    //
    //   // vehicle is stopping or not
    //   double stopping_threshold = 1.0;
    //   g_current_vel_kmph < stopping_threshold ? stop_count++ : stop_count = 0;
    //
    //   // std::cout << "stop count: " << stop_count << std::endl;
    //   // vehicle has stopped, so we can restart
    //   if (stop_count > g_loop_rate * g_stop_time_sec)
    //   {
    //     brake = 0;
    //     stop_count = 0;
    //     stopping_flag = false;
    //   }
    // }

    // Insert data to 8 byte array
    unsigned char data[8] = {};
    ymc::setCanData(data, mode, shift, target_velocity_ui16, steering_angle_deg_i16, brake, heart_beat);

    // send can data
    std::string send_id("200");
    size_t data_size = sizeof(data) / sizeof(data[0]);
    char can_cmd[256];
    std::strcpy(can_cmd, mycansend::makeCmdArgument(data, data_size, send_id).c_str());
    g_cansender.send(can_cmd);

    // wait for callbacks
    ros::spinOnce();
    loop_rate.sleep();

    heart_beat += 1;
  }

  g_terminate_thread = true;
  t1.join();
  t2.join();

  pclose(fp);

  return 0;
}
