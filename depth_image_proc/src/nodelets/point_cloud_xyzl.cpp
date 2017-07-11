/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2017, Kentar Wada
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Kentaro Wada nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/
#include <limits>
#include <string>

#include <boost/version.hpp>
#if ((BOOST_VERSION / 100) % 1000) >= 53
#include <boost/thread/lock_guard.hpp>
#endif

#include <ros/ros.h>
#include <nodelet/nodelet.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/point_cloud2_iterator.h>
#include <sensor_msgs/PointCloud2.h>
#include <image_geometry/pinhole_camera_model.h>
#include <depth_image_proc/depth_traits.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/imgproc/imgproc.hpp>

namespace depth_image_proc
{

namespace enc = sensor_msgs::image_encodings;

class PointCloudXyzlNodelet : public nodelet::Nodelet
{
  ros::NodeHandlePtr label_nh_;
  boost::shared_ptr<image_transport::ImageTransport> label_it_, depth_it_;

  // Subscriptions
  image_transport::SubscriberFilter sub_depth_, sub_label_;
  message_filters::Subscriber<sensor_msgs::CameraInfo> sub_info_;
  typedef message_filters::sync_policies::ApproximateTime<
    sensor_msgs::Image,
    sensor_msgs::Image,
    sensor_msgs::CameraInfo> SyncPolicy;
  typedef message_filters::Synchronizer<SyncPolicy> Synchronizer;
  boost::shared_ptr<Synchronizer> sync_;

  // Publications
  boost::mutex connect_mutex_;
  typedef sensor_msgs::PointCloud2 PointCloud;
  ros::Publisher pub_point_cloud_;

  image_geometry::PinholeCameraModel model_;

  virtual void onInit();

  void connectCb();

  void imageCb(const sensor_msgs::ImageConstPtr& depth_msg,
               const sensor_msgs::ImageConstPtr& label_msg,
               const sensor_msgs::CameraInfoConstPtr& info_msg);

  template<typename T, typename T2>
  void convert(const sensor_msgs::ImageConstPtr& depth_msg,
               const sensor_msgs::ImageConstPtr& label_msg,
               const PointCloud::Ptr& cloud_msg);
};

void PointCloudXyzlNodelet::onInit()
{
  ros::NodeHandle& nh         = getNodeHandle();
  ros::NodeHandle& private_nh = getPrivateNodeHandle();
  label_nh_.reset( new ros::NodeHandle(nh, "label") );
  ros::NodeHandle depth_nh(nh, "depth_registered");
  label_it_  .reset( new image_transport::ImageTransport(*label_nh_) );
  depth_it_.reset( new image_transport::ImageTransport(depth_nh) );

  // Read parameters
  int queue_size;
  private_nh.param("queue_size", queue_size, 5);

  // Synchronize inputs. Topic subscriptions happen on demand in the connection callback.
  sync_.reset( new Synchronizer(SyncPolicy(queue_size), sub_depth_, sub_label_, sub_info_) );
  sync_->registerCallback(boost::bind(&PointCloudXyzlNodelet::imageCb, this, _1, _2, _3));

  // Monitor whether anyone is subscribed to the output
  ros::SubscriberStatusCallback connect_cb = boost::bind(&PointCloudXyzlNodelet::connectCb, this);
  // Make sure we don't enter connectCb() between advertising and assigning to pub_point_cloud_
  boost::lock_guard<boost::mutex> lock(connect_mutex_);
  pub_point_cloud_ = depth_nh.advertise<PointCloud>("points", 1, connect_cb, connect_cb);
}

// Handles (un)subscribing when clients (un)subscribe
void PointCloudXyzlNodelet::connectCb()
{
  boost::lock_guard<boost::mutex> lock(connect_mutex_);
  if (pub_point_cloud_.getNumSubscribers() == 0)
  {
    sub_depth_.unsubscribe();
    sub_label_.unsubscribe();
    sub_info_.unsubscribe();
  }
  else if (!sub_depth_.getSubscriber())
  {
    ros::NodeHandle& private_nh = getPrivateNodeHandle();
    // parameter for depth_image_transport hint
    std::string depth_image_transport_param = "depth_image_transport";

    // depth image can use different transport.(e.g. compressedDepth)
    image_transport::TransportHints depth_hints("raw", ros::TransportHints(), private_nh, depth_image_transport_param);
    sub_depth_.subscribe(*depth_it_, "image_rect",       1, depth_hints);

    // label uses normal ros transport hints.
    image_transport::TransportHints hints("raw", ros::TransportHints(), private_nh);
    sub_label_.subscribe(*label_it_,   "label", 1, hints);
    sub_info_.subscribe(*label_nh_,   "camera_info",      1);
  }
}

void PointCloudXyzlNodelet::imageCb(const sensor_msgs::ImageConstPtr& depth_msg,
                                      const sensor_msgs::ImageConstPtr& label_msg_in,
                                      const sensor_msgs::CameraInfoConstPtr& info_msg)
{
  // Check for bad inputs
  if (depth_msg->header.frame_id != label_msg_in->header.frame_id)
  {
    NODELET_ERROR_THROTTLE(5, "Depth image frame id [%s] doesn't match image frame id [%s]",
                           depth_msg->header.frame_id.c_str(), label_msg_in->header.frame_id.c_str());
    return;
  }

  // Update camera model
  model_.fromCameraInfo(info_msg);

  // Check if the input image has to be resized
  sensor_msgs::ImageConstPtr label_msg = label_msg_in;
  if (depth_msg->width != label_msg->width || depth_msg->height != label_msg->height)
  {
    sensor_msgs::CameraInfo info_msg_tmp = *info_msg;
    info_msg_tmp.width = depth_msg->width;
    info_msg_tmp.height = depth_msg->height;
    float ratio = static_cast<float>(depth_msg->width) / static_cast<float>(label_msg->width);
    info_msg_tmp.K[0] *= ratio;
    info_msg_tmp.K[2] *= ratio;
    info_msg_tmp.K[4] *= ratio;
    info_msg_tmp.K[5] *= ratio;
    info_msg_tmp.P[0] *= ratio;
    info_msg_tmp.P[2] *= ratio;
    info_msg_tmp.P[5] *= ratio;
    info_msg_tmp.P[6] *= ratio;
    model_.fromCameraInfo(info_msg_tmp);

    cv_bridge::CvImageConstPtr cv_ptr;
    try
    {
      cv_ptr = cv_bridge::toCvShare(label_msg, label_msg->encoding);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
    }
    cv_bridge::CvImage cv_rsz;
    cv_rsz.header = cv_ptr->header;
    cv_rsz.encoding = cv_ptr->encoding;
    cv::resize(cv_ptr->image.rowRange(0, depth_msg->height / ratio),
               cv_rsz.image,
               cv::Size(depth_msg->width, depth_msg->height),
               cv::INTER_NEAREST);
    label_msg = cv_rsz.toImageMsg();
  }
  else
  {
    label_msg = label_msg_in;
  }

  // Supported label encodings: 32SC1, 8UC1
  if (label_msg->encoding != enc::TYPE_32SC1 || label_msg->encoding != enc::TYPE_8UC1)
  {
    try
    {
      label_msg = cv_bridge::toCvCopy(label_msg, enc::TYPE_32SC1)->toImageMsg();
    }
    catch (cv_bridge::Exception& e)
    {
      NODELET_ERROR_THROTTLE(5, "Unsupported encoding [%s]: %s", label_msg->encoding.c_str(), e.what());
      return;
    }
  }

  // Allocate new point cloud message
  PointCloud::Ptr cloud_msg (new PointCloud);
  cloud_msg->header = depth_msg->header;  // Use depth image time stamp
  cloud_msg->height = depth_msg->height;
  cloud_msg->width  = depth_msg->width;
  cloud_msg->is_dense = false;
  cloud_msg->is_bigendian = false;

  sensor_msgs::PointCloud2Modifier pcd_modifier(*cloud_msg);
  pcd_modifier.setPointCloud2Fields(4,
    "x", 1, sensor_msgs::PointField::FLOAT32,
    "y", 1, sensor_msgs::PointField::FLOAT32,
    "z", 1, sensor_msgs::PointField::FLOAT32,
    "label", 1, sensor_msgs::PointField::UINT32);


  if (depth_msg->encoding == enc::TYPE_16UC1 &&
      label_msg->encoding == enc::TYPE_32SC1)
  {
    convert<uint16_t, int32_t>(depth_msg, label_msg, cloud_msg);
  }
  else if (depth_msg->encoding == enc::TYPE_16UC1 &&
           label_msg->encoding == enc::MONO8)
  {
    convert<uint16_t, uint8_t>(depth_msg, label_msg, cloud_msg);
  }
  else if (depth_msg->encoding == enc::TYPE_32FC1 &&
           label_msg->encoding == enc::TYPE_32SC1)
  {
    convert<float, int32_t>(depth_msg, label_msg, cloud_msg);
  }
  else if (depth_msg->encoding == enc::TYPE_32FC1 &&
           label_msg->encoding == enc::MONO8)
  {
    convert<float, uint8_t>(depth_msg, label_msg, cloud_msg);
  }
  else
  {
    NODELET_ERROR_THROTTLE(5, "Depth image has unsupported encoding [%s]", depth_msg->encoding.c_str());
    return;
  }

  pub_point_cloud_.publish(cloud_msg);
}

template<typename T, typename T2>
void PointCloudXyzlNodelet::convert(const sensor_msgs::ImageConstPtr& depth_msg,
                                    const sensor_msgs::ImageConstPtr& label_msg,
                                    const PointCloud::Ptr& cloud_msg)
{
  // Use correct principal point from calibration
  float center_x = model_.cx();
  float center_y = model_.cy();

  // Combine unit conversion (if necessary) with scaling by focal length for computing (X,Y)
  double unit_scaling = DepthTraits<T>::toMeters(T(1));
  float constant_x = unit_scaling / model_.fx();
  float constant_y = unit_scaling / model_.fy();
  float bad_point = std::numeric_limits<float>::quiet_NaN();

  const T* depth_row = reinterpret_cast<const T*>(&depth_msg->data[0]);
  int row_step = depth_msg->step / sizeof(T);

  const T2* label_row = reinterpret_cast<const T2*>(&label_msg->data[0]);
  int label_row_step  = label_msg->step / sizeof(T2);

  sensor_msgs::PointCloud2Iterator<float> iter_x(*cloud_msg, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(*cloud_msg, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(*cloud_msg, "z");
  sensor_msgs::PointCloud2Iterator<uint32_t> iter_l(*cloud_msg, "label");

  for (int v = 0; v < static_cast<int>(cloud_msg->height); ++v, depth_row += row_step, label_row += label_row_step)
  {
    for (int u = 0; u < static_cast<int>(cloud_msg->width); ++u, ++iter_x, ++iter_y, ++iter_z, ++iter_l)
    {
      T depth = depth_row[u];
      T2 label = label_row[u];
      // Check for invalid measurements
      if (!DepthTraits<T>::valid(depth) || label < 0)
      {
        *iter_x = *iter_y = *iter_z = bad_point;
      }
      else
      {
        // Fill in XYZ
        *iter_x = (u - center_x) * depth * constant_x;
        *iter_y = (v - center_y) * depth * constant_y;
        *iter_z = DepthTraits<T>::toMeters(depth);
      }

      // Fill in label
      *iter_l = label;
    }
  }
}

}  // namespace depth_image_proc

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(depth_image_proc::PointCloudXyzlNodelet, nodelet::Nodelet);
