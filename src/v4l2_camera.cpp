// Copyright 2019 Bold Hearts
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "v4l2_camera/v4l2_camera.hpp"

#include <sensor_msgs/image_encodings.hpp>
#include <cv_bridge/cv_bridge.h>

#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <algorithm>

#include "rclcpp_components/register_node_macro.hpp"

using namespace std::chrono_literals;

namespace v4l2_camera
{

V4L2Camera::V4L2Camera(rclcpp::NodeOptions const & options)
: rclcpp::Node{"v4l2_camera", options},
  canceled_{false}
{
  // Prepare camera
  auto device = declare_parameter<std::string>("video_device", "/dev/video0");
  RCLCPP_INFO(get_logger(), "Using video device: " + device);

  camera_ = std::make_shared<V4l2CameraDevice>(device);

  if (!camera_->open()) {
    return;
  }

  // Request pixel format
  auto pixel_format = declare_parameter<std::string>("pixel_format", "YUYV");
  requestPixelFormat(pixel_format);

  cinfo_ = std::make_shared<camera_info_manager::CameraInfoManager>(this, camera_->getCameraName());

  // Read parameters and set up callback
  createParameters();

  // Prepare publisher
  if (options.use_intra_process_comms()) {
    image_pub_ = create_publisher<sensor_msgs::msg::Image>("/image_raw", 10);
  } else {
    camera_transport_pub_ = image_transport::create_camera_publisher(this, "/image_raw");
  }

  // Start the camera
  if (!camera_->start()) {
    return;
  }

  // Start capture thread
  capture_thread_ = std::thread{
    [this]() -> void {
      while (rclcpp::ok() && !canceled_.load()) {
        RCLCPP_DEBUG(get_logger(), "Capture...");
        auto img = camera_->capture();
        auto stamp = now();
        if (img->encoding != output_encoding_) {
          RCLCPP_WARN_ONCE(get_logger(),
            "Image encoding not same as requested output, converting: " +
            img->encoding + " => " + output_encoding_);
          img = convert(*img);
        }
        img->header.stamp = stamp;
        img->header.frame_id = camera_frame_id_;

        if (get_node_options().use_intra_process_comms()) {
          std::stringstream ss;
          ss << "Image message address [PUBLISH]:\t" << img.get();
          RCLCPP_DEBUG(get_logger(), ss.str());
          image_pub_->publish(std::move(img));
        } else {
          auto ci = cinfo_->getCameraInfo();
          if (!checkCameraInfo(*img, ci)) {
            ci = sensor_msgs::msg::CameraInfo{};
            ci.height = img->height;
            ci.width = img->width;
          }

          ci.header.stamp = stamp;

          camera_transport_pub_.publish(*img, ci);
        }
      }
    }
  };
}

V4L2Camera::~V4L2Camera()
{
  canceled_.store(true);
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
}

void V4L2Camera::createParameters()
{
  // Node parameters
  output_encoding_ = declare_parameter("output_encoding", std::string{"rgb8"});

  // Camera info parameters
  auto camera_info_url = std::string{};
  if (get_parameter("camera_info_url", camera_info_url)) {
    if (cinfo_->validateURL(camera_info_url)) {
      cinfo_->loadCameraInfo(camera_info_url);
    } else {
      RCLCPP_WARN(get_logger(), std::string{"Invalid camera info URL: "} + camera_info_url);
    }
  }

  camera_frame_id_ = declare_parameter<std::string>("camera_frame_id", "camera");

  // Format parameters
  using ImageSize = std::vector<int64_t>;
  auto image_size = ImageSize{};
  image_size = declare_parameter<ImageSize>("image_size", {640, 480});
  requestImageSize(image_size);

  // Control parameters
  auto toParamName =
    [](std::string name) {
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);
      name.erase(std::remove(name.begin(), name.end(), ','), name.end());
      name.erase(std::remove(name.begin(), name.end(), '('), name.end());
      name.erase(std::remove(name.begin(), name.end(), ')'), name.end());
      std::replace(name.begin(), name.end(), ' ', '_');
      return name;
    };

  for (auto const & c : camera_->getControls()) {
    auto name = toParamName(c.name);
    switch (c.type) {
      case ControlType::INT:
        {
          auto value = declare_parameter<int64_t>(name, camera_->getControlValue(c.id));
          camera_->setControlValue(c.id, value);
          break;
        }
      case ControlType::BOOL:
        {
          auto value = declare_parameter<bool>(name, camera_->getControlValue(c.id) != 0);
          camera_->setControlValue(c.id, value);
          break;
        }
      case ControlType::MENU:
        {
          // TODO(sander): treating as integer parameter, implement full menu functionality
          auto value = declare_parameter<int64_t>(name, camera_->getControlValue(c.id));
          camera_->setControlValue(c.id, value);
          break;
        }
      default:
        RCLCPP_WARN(get_logger(),
          std::string{"Control type not currently supported: "} + std::to_string(unsigned(c.type)) +
          ", for controle: " + c.name);
        continue;
    }
    control_name_to_id_[name] = c.id;
  }

  // Register callback for parameter value setting
  set_on_parameters_set_callback(
    [this](std::vector<rclcpp::Parameter> parameters) -> rcl_interfaces::msg::SetParametersResult {
      auto result = rcl_interfaces::msg::SetParametersResult();
      result.successful = true;
      for (auto const & p : parameters) {
        result.successful &= handleParameter(p);
      }
      return result;
    });
}

bool V4L2Camera::handleParameter(rclcpp::Parameter const & param)
{
  auto name = std::string{param.get_name()};
  if (control_name_to_id_.find(name) != control_name_to_id_.end()) {
    switch (param.get_type()) {
      case rclcpp::ParameterType::PARAMETER_BOOL:
        return camera_->setControlValue(control_name_to_id_[name], param.as_bool());
      case rclcpp::ParameterType::PARAMETER_INTEGER:
        return camera_->setControlValue(control_name_to_id_[name], param.as_int());
      default:
        RCLCPP_WARN(get_logger(),
          std::string{"Control parameter type not currently supported: "} +
          std::to_string(unsigned(param.get_type())) +
          ", for parameter: " + param.get_name());
    }
  } else if (param.get_name() == "output_encoding") {
    output_encoding_ = param.as_string();
    return true;
  } else if (param.get_name() == "size") {
    camera_->stop();
    auto success = requestImageSize(param.as_integer_array());
    camera_->start();
    return success;
  } else if (param.get_name() == "camera_info_url") {
    auto camera_info_url = param.as_string();
    if (cinfo_->validateURL(camera_info_url)) {
      return cinfo_->loadCameraInfo(camera_info_url);
    } else {
      RCLCPP_WARN(get_logger(), std::string{"Invalid camera info URL: "} + camera_info_url);
      return false;
    }
  }

  return false;
}

bool V4L2Camera::requestPixelFormat(std::string const & fourcc)
{
  if (fourcc.size() != 4) {
    RCLCPP_ERROR(get_logger(), "Invalid pixel format size: must be a 4 character code (FOURCC).");
    return false;
  }

  auto code = v4l2_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);

  auto dataFormat = camera_->getCurrentDataFormat();
  // Do not apply if camera already runs at given pixel format
  if (dataFormat.pixelFormat == code) {
    return true;
  }

  dataFormat.pixelFormat = code;
  return camera_->requestDataFormat(dataFormat);
}

bool V4L2Camera::requestImageSize(std::vector<int64_t> const & size)
{
  if (size.size() != 2) {
    RCLCPP_WARN(
      get_logger(),
      "Invalid image size; expected dimensions: 2, actual: " + std::to_string(size.size()));
    return false;
  }

  auto dataFormat = camera_->getCurrentDataFormat();
  // Do not apply if camera already runs at given size
  if (dataFormat.width == size[0] && dataFormat.height == size[1]) {
    return true;
  }

  dataFormat.width = size[0];
  dataFormat.height = size[1];
  return camera_->requestDataFormat(dataFormat);
}

sensor_msgs::msg::Image::UniquePtr V4L2Camera::convert(sensor_msgs::msg::Image const & img) const
{
  RCLCPP_DEBUG(get_logger(),
    std::string{"Coverting: "} + img.encoding + " -> " + output_encoding_);

  auto tracked_object = std::shared_ptr<const void>{};
  auto cvImg = cv_bridge::toCvShare(img, tracked_object);
  auto outImg = std::make_unique<sensor_msgs::msg::Image>();
  auto cvConvertedImg = cv_bridge::cvtColor(cvImg, output_encoding_);
  cvConvertedImg->toImageMsg(*outImg);
  return outImg;
}

bool V4L2Camera::checkCameraInfo(
  sensor_msgs::msg::Image const & img,
  sensor_msgs::msg::CameraInfo const & ci)
{
  return ci.width == img.width && ci.height == img.height;
}

}  // namespace v4l2_camera

RCLCPP_COMPONENTS_REGISTER_NODE(v4l2_camera::V4L2Camera)
