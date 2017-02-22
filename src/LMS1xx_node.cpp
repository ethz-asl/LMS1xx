/*
 * LMS1xx.cpp
 *
 *  Created on: 09-08-2010
 *  Author: Konrad Banachowicz
 ***************************************************************************
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Lesser General Public            *
 *   License as published by the Free Software Foundation; either          *
 *   version 2.1 of the License, or (at your option) any later version.    *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/

#include <csignal>
#include <cstdio>
#include <sm/timing/TimestampCorrector.hpp>
#include <LMS1xx/LMS1xx.h>
#include "ros/ros.h"
#include "sensor_msgs/LaserScan.h"

#define DEG2RAD M_PI/180.0

int main(int argc, char **argv)
{
  // laser data
  LMS1xx laser;
  scanCfg cfg;
  scanOutputRange outputRange;
  scanDataCfg dataCfg;
  sensor_msgs::LaserScan scan_msg;

  sm::timing::TimestampCorrector<double> timestampCorrector;

  // parameters
  std::string host;
  std::string frame_id;
  int port;
  bool use_hwtime;

  ros::init(argc, argv, "lms1xx");
  ros::NodeHandle nh;
  ros::NodeHandle n("~");
  ros::Publisher scan_pub = nh.advertise<sensor_msgs::LaserScan>("scan", 1);

  n.param<std::string>("host", host, "192.168.1.2");
  n.param<std::string>("frame_id", frame_id, "laser");
  n.param("use_hwtime", use_hwtime, false);
  n.param<int>("port", port, 2111);

  if(use_hwtime){
    ROS_INFO_STREAM("Going to use hardware timestamps. Experimental!");
  } else {
    ROS_INFO_STREAM("NOT Going to use hardware timestamps.");
  }

  while (ros::ok())
  {
    ROS_INFO_STREAM("Connecting to laser at " << host);
    laser.connect(host, port);
    if (!laser.isConnected())
    {
      ROS_WARN("Unable to connect, retrying.");
      ros::Duration(1).sleep();
      continue;
    }

    ROS_DEBUG("Logging in to laser.");
    laser.login();
    cfg = laser.getScanCfg();
    outputRange = laser.getScanOutputRange();

    if (cfg.scaningFrequency != 5000)
    {
      laser.disconnect();
      ROS_WARN("Unable to get laser output range. Retrying.");
      ros::Duration(1).sleep();
      continue;
    }

    ROS_INFO("Connected to laser.");

    ROS_DEBUG("Laser configuration: scaningFrequency %d, angleResolution %d, startAngle %d, stopAngle %d",
              cfg.scaningFrequency, cfg.angleResolution, cfg.startAngle, cfg.stopAngle);
    ROS_DEBUG("Laser output range:angleResolution %d, startAngle %d, stopAngle %d",
              outputRange.angleResolution, outputRange.startAngle, outputRange.stopAngle);

    scan_msg.header.frame_id = frame_id;
    scan_msg.range_min = 0.01;
    scan_msg.range_max = 20.0;
    scan_msg.scan_time = 100.0 / cfg.scaningFrequency;
    scan_msg.angle_increment = (double)outputRange.angleResolution / 10000.0 * DEG2RAD;
    scan_msg.angle_min = (double)outputRange.startAngle / 10000.0 * DEG2RAD - M_PI / 2;
    scan_msg.angle_max = (double)outputRange.stopAngle / 10000.0 * DEG2RAD - M_PI / 2;

    ROS_DEBUG_STREAM("Device resolution is " << (double)outputRange.angleResolution / 10000.0 << " degrees.");
    ROS_DEBUG_STREAM("Device frequency is " << (double)cfg.scaningFrequency / 100.0 << " Hz");

    int angle_range = outputRange.stopAngle - outputRange.startAngle;
    int num_values = angle_range / outputRange.angleResolution ;
    if (angle_range % outputRange.angleResolution == 0)
    {
      // Include endpoint
      ++num_values;
    }
    scan_msg.ranges.resize(num_values);
    scan_msg.intensities.resize(num_values);

    scan_msg.time_increment =
      (outputRange.angleResolution / 10000.0)
      / 360.0
      / (cfg.scaningFrequency / 100.0);

    ROS_DEBUG_STREAM("Time increment is " << static_cast<int>(scan_msg.time_increment * 1000000) << " microseconds");

    dataCfg.outputChannel = 1;
    dataCfg.remission = true;
    dataCfg.resolution = 1;
    dataCfg.encoder = 0;
    dataCfg.position = false;
    dataCfg.deviceName = false;
    dataCfg.outputInterval = 1;
    dataCfg.timestamp = true;

    ROS_DEBUG("Setting scan data configuration.");
    laser.setScanDataCfg(dataCfg);

    ROS_DEBUG("Starting measurements.");
    laser.startMeas();

    ROS_DEBUG("Waiting for ready status.");
    ros::Time ready_status_timeout = ros::Time::now() + ros::Duration(5);

    //while(1)
    //{
    status_t stat = laser.queryStatus();
    ros::Duration(1.0).sleep();
    if (stat != ready_for_measurement)
    {
      ROS_WARN("Laser not ready. Retrying initialization.");
      laser.disconnect();
      ros::Duration(1).sleep();
      continue;
    }
    /*if (stat == ready_for_measurement)
    {
      ROS_DEBUG("Ready status achieved.");
      break;
    }

      if (ros::Time::now() > ready_status_timeout)
      {
        ROS_WARN("Timed out waiting for ready status. Trying again.");
        laser.disconnect();
        continue;
      }

      if (!ros::ok())
      {
        laser.disconnect();
        return 1;
      }
    }*/

    ROS_DEBUG("Starting device.");
    laser.startDevice(); // Log out to properly re-enable system after config

    ROS_DEBUG("Commanding continuous measurements.");
    laser.scanContinous(1);


    struct WrapFixer {
      uint32_t hwSecs = 0, lastHwUsecs = -1;

      void update (uint32_t hwUsecs) {
        if (lastHwUsecs > hwUsecs) {
          hwSecs ++;
        }
        lastHwUsecs = hwUsecs;
      }
      uint64_t getNSecs() {
        return hwSecs * uint64_t(1e9) + lastHwUsecs * 1000;
      }

      double getDouble() {
        return (double)hwSecs + lastHwUsecs * 1e-6;
      }
    } hw_transmit, hw_start;

    while (ros::ok())
    {
      ++scan_msg.header.seq;

      scanData data;
      ROS_DEBUG("Reading scan data.");
      if (laser.getScanData(&data))
      {
        if(use_hwtime){
          hw_start.update(data.hw_stamp_usec);
          hw_transmit.update(data.hw_transmit_stamp_usec);
          const int timestampLearnLimit = -1;
          if (timestampLearnLimit < 0 || scan_msg.header.seq < timestampLearnLimit) {
            timestampCorrector.correctTimestamp(hw_transmit.getDouble(), data.receive_ros_time.toSec());
          } else if(scan_msg.header.seq == timestampLearnLimit){
            std::cout << "timestampCorrector.getSlope()=" << timestampCorrector.getSlope() << std::endl; // XXX: debug output of timestampCorrector.getSlope((
          }

          if(scan_msg.header.seq > 1){
            double local = timestampCorrector.getLocalTime(hw_start.getDouble());
            if(timestampLearnLimit > 0 && scan_msg.header.seq >= timestampLearnLimit){
              local = timestampCorrector.getOffset() + timestampCorrector.getSlope() * hw_start.getDouble();
            }
            scan_msg.header.stamp.fromSec(local);

          } else {
            scan_msg.header.stamp = data.receive_ros_time;
          }
          ROS_DEBUG("Hwtime: %u mapped to %lu", data.hw_stamp_usec, scan_msg.header.stamp.toNSec());
        } else {
          scan_msg.header.stamp = data.receive_ros_time;
          ROS_DEBUG("Rostime : %lu", scan_msg.header.stamp.toNSec());
        }

        for (int i = 0; i < data.dist_len1; i++)
        {
          scan_msg.ranges[i] = data.dist1[i] * 0.001;
        }

        for (int i = 0; i < data.rssi_len1; i++)
        {
          scan_msg.intensities[i] = data.rssi1[i];
        }

        ROS_DEBUG("Publishing scan data.");
        scan_pub.publish(scan_msg);
      }
      else
      {
        ROS_ERROR("Laser timed out on delivering scan, attempting to reinitialize.");
        break;
      }

      ros::spinOnce();
    }

    laser.scanContinous(0);
    laser.stopMeas();
    laser.disconnect();
  }

  return 0;
}
