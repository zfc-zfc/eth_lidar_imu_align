#include <geometry_msgs/TransformStamped.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <sensor_msgs/Imu.h>
#include "lidar_align/loader.h"
#include "lidar_align/transform.h"

namespace lidar_align {

Loader::Loader(const Config& config) : config_(config) {}

Loader::Config Loader::getConfig(ros::NodeHandle* nh) {
  Loader::Config config;
  nh->param("use_n_scans", config.use_n_scans, config.use_n_scans);
  return config;
}

void Loader::parsePointcloudMsg(const sensor_msgs::PointCloud2 msg,
                                LoaderPointcloud* pointcloud) {
  bool has_timing = false;
  bool has_intensity = false;
  for (const sensor_msgs::PointField& field : msg.fields) {
    if (field.name == "time_offset_us") {   //需要名为time_offset_us，单位为微秒的field.
      has_timing = true;
    } else if (field.name == "intensity") {
      has_intensity = true;
    }
  }

  if (has_timing) {
    pcl::fromROSMsg(msg, *pointcloud);
    return;
  } else if (has_intensity) {
    Pointcloud raw_pointcloud;
    pcl::fromROSMsg(msg, raw_pointcloud);

    for (const Point& raw_point : raw_pointcloud) {
      PointAllFields point;
      point.x = raw_point.x;
      point.y = raw_point.y;
      point.z = raw_point.z;
      point.intensity = raw_point.intensity;

      if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z) || !std::isfinite(point.intensity)) {
        continue;
      }

      pointcloud->push_back(point);
    }
    pointcloud->header = raw_pointcloud.header;
  } else {
    pcl::PointCloud<pcl::PointXYZ> raw_pointcloud;
    pcl::fromROSMsg(msg, raw_pointcloud);

    for (const pcl::PointXYZ& raw_point : raw_pointcloud) {
      PointAllFields point;
      point.x = raw_point.x;
      point.y = raw_point.y;
      point.z = raw_point.z;

      if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z)) {
        continue;
      }

      pointcloud->push_back(point);
    }
    pointcloud->header = raw_pointcloud.header;
  }
}

void parsePointcloudMsg_ouster(const sensor_msgs::PointCloud2 msg,
                               LoaderPointcloud* pointcloud){
    bool has_timing = false;
    bool has_intensity = false;
    for (const sensor_msgs::PointField& field : msg.fields) {
        if (field.name == "t") {   //需要名为t，单位为微秒的field.
            has_timing = true;
        } else if (field.name == "intensity") {
            has_intensity = true;
        }
    }

    if(has_timing && has_intensity){
        pcl::PointCloud<Ouster_Point> raw_pointcloud;
        pcl::fromROSMsg(msg, raw_pointcloud);

        for (const Ouster_Point& raw_point : raw_pointcloud) {
            PointAllFields point;
            point.x = raw_point.x;
            point.y = raw_point.y;
            point.z = raw_point.z;
            point.intensity = raw_point.intensity;
            point.time_offset_us = raw_point.t / 1e3;   //ns to us
            point.ring = raw_point.ring;

            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
                !std::isfinite(point.intensity) || !std::isfinite(point.time_offset_us)) {
                continue;
            }
            pointcloud->push_back(point);
        }
        pointcloud->header = raw_pointcloud.header;
        return;
    }else{
        pcl::PointCloud<pcl::PointXYZ> raw_pointcloud;
        pcl::fromROSMsg(msg, raw_pointcloud);

        for (const pcl::PointXYZ& raw_point : raw_pointcloud) {
            PointAllFields point;
            point.x = raw_point.x;
            point.y = raw_point.y;
            point.z = raw_point.z;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                continue;
            }
            pointcloud->push_back(point);
        }
        pointcloud->header = raw_pointcloud.header;
    }
}



bool Loader::loadPointcloudFromROSBag(const std::string& bag_path,
                                      const Scan::Config& scan_config,
                                      Lidar* lidar) {
  rosbag::Bag bag;
  try {
    bag.open(bag_path, rosbag::bagmode::Read);
  } catch (rosbag::BagException e) {
    ROS_ERROR_STREAM("LOADING BAG FAILED: " << e.what());
    return false;
  }

  std::vector<std::string> types;
  types.push_back(std::string("sensor_msgs/PointCloud2"));
  rosbag::View view(bag, rosbag::TypeQuery(types));

  size_t scan_num = 0;
  for (const rosbag::MessageInstance& m : view) {
    std::cout << " Loading scan: \e[1m" << scan_num++ << "\e[0m from ros bag"
              << '\r' << std::flush;

    LoaderPointcloud pointcloud;
    parsePointcloudMsg(*(m.instantiate<sensor_msgs::PointCloud2>()),
                       &pointcloud);

    //parsePointcloudMsg_ouster(*(m.instantiate<sensor_msgs::PointCloud2>()), &pointcloud);

    lidar->addPointcloud(pointcloud, scan_config);

    if (lidar->getNumberOfScans() >= config_.use_n_scans) {
      break;
    }
  }
  if (lidar->getTotalPoints() == 0) {
    ROS_ERROR_STREAM(
        "No points were loaded, verify that the bag contains populated "
        "messages of type sensor_msgs/PointCloud2");
    return false;
  }
  return true;
}

bool Loader::loadTformFromROSBag(const std::string& bag_path, Odom* odom) {
  rosbag::Bag bag;
  try {
    bag.open(bag_path, rosbag::bagmode::Read);
  } catch (rosbag::BagException e) {
    ROS_ERROR_STREAM("LOADING BAG FAILED: " << e.what());
    return false;
  }

  std::vector<std::string> types;

  ///读取IMU数据
  types.push_back(std::string("sensor_msgs/Imu"));
  rosbag::View view(bag, rosbag::TypeQuery(types));
  size_t imu_num = 0;
  double shiftX=0,shiftY=0,shiftZ=0,velX=0,velY=0,velZ=0;
  ros::Time time;
  double timeDiff,lastShiftX,lastShiftY,lastShiftZ;
  for (const rosbag::MessageInstance& m : view){
      std::cout <<"Loading imu: \e[1m"<< imu_num++<<"\e[0m from ros bag"<<'\r'<< std::flush;
      sensor_msgs::Imu imu=*(m.instantiate<sensor_msgs::Imu>());
      Timestamp stamp = imu.header.stamp.sec * 1000000ll +imu.header.stamp.nsec / 1000ll;
      if(imu_num==1){
          time=imu.header.stamp;
          Transform T(Transform::Translation(0,0,0),Transform::Rotation(1,0,0,0));
          odom->addTransformData(stamp, T);
      }
      else{
          timeDiff=(imu.header.stamp-time).toSec();
          time=imu.header.stamp;
          velX=velX+imu.linear_acceleration.x*timeDiff;
          velY=velX+imu.linear_acceleration.y*timeDiff;
          velZ=velZ+(imu.linear_acceleration.z-9.801)*timeDiff;
          lastShiftX=shiftX;
          lastShiftY=shiftY;
          lastShiftZ=shiftZ;
          shiftX=lastShiftX+velX*timeDiff+imu.linear_acceleration.x*timeDiff*timeDiff/2;
          shiftY=lastShiftY+velY*timeDiff+imu.linear_acceleration.y*timeDiff*timeDiff/2;
          shiftZ=lastShiftZ+velZ*timeDiff+(imu.linear_acceleration.z-9.801)*timeDiff*timeDiff/2;
          Transform T(Transform::Translation(shiftX,shiftY,shiftZ),
                      Transform::Rotation(imu.orientation.w,
                                          imu.orientation.x,
                                          imu.orientation.y,
                                          imu.orientation.z));
          odom->addTransformData(stamp, T);
      }
  }

  ///读取TransformStamped类型
  /*
  types.push_back(std::string("geometry_msgs/TransformStamped"));
  rosbag::View view(bag, rosbag::TypeQuery(types));

  size_t tform_num = 0;
  for (const rosbag::MessageInstance& m : view) {
    std::cout << " Loading transform: \e[1m" << tform_num++
              << "\e[0m from ros bag" << '\r' << std::flush;

    geometry_msgs::TransformStamped transform_msg =
        *(m.instantiate<geometry_msgs::TransformStamped>());

    Timestamp stamp = transform_msg.header.stamp.sec * 1000000ll +
                      transform_msg.header.stamp.nsec / 1000ll;

    Transform T(Transform::Translation(transform_msg.transform.translation.x,
                                       transform_msg.transform.translation.y,
                                       transform_msg.transform.translation.z),
                Transform::Rotation(transform_msg.transform.rotation.w,
                                    transform_msg.transform.rotation.x,
                                    transform_msg.transform.rotation.y,
                                    transform_msg.transform.rotation.z));
    odom->addTransformData(stamp, T);
  }
   */

  if (odom->empty()) {
    ROS_ERROR_STREAM("No odom messages found!");
    return false;
  }
  return true;
}

bool Loader::loadTformFromMaplabCSV(const std::string& csv_path, Odom* odom) {
  std::ifstream file(csv_path, std::ifstream::in);

  size_t tform_num = 0;
  while (file.peek() != EOF) {
    std::cout << " Loading transform: \e[1m" << tform_num++
              << "\e[0m from csv file" << '\r' << std::flush;

    Timestamp stamp;
    Transform T;

    if (getNextCSVTransform(file, &stamp, &T)) {
      odom->addTransformData(stamp, T);
    }
  }

  return true;
}

// lots of potential failure cases not checked
bool Loader::getNextCSVTransform(std::istream& str, Timestamp* stamp,
                                 Transform* T) {
  std::string line;
  std::getline(str, line);

  // ignore comment lines
  if (line[0] == '#') {
    return false;
  }

  std::stringstream line_stream(line);
  std::string cell;

  std::vector<std::string> data;
  while (std::getline(line_stream, cell, ',')) {
    data.push_back(cell);
  }

  if (data.size() < 9) {
    return false;
  }

  constexpr size_t TIME = 0;
  constexpr size_t X = 2;
  constexpr size_t Y = 3;
  constexpr size_t Z = 4;
  constexpr size_t RW = 5;
  constexpr size_t RX = 6;
  constexpr size_t RY = 7;
  constexpr size_t RZ = 8;

  *stamp = std::stoll(data[TIME]) / 1000ll;
  *T = Transform(Transform::Translation(std::stod(data[X]), std::stod(data[Y]),
                                        std::stod(data[Z])),
                 Transform::Rotation(std::stod(data[RW]), std::stod(data[RX]),
                                     std::stod(data[RY]), std::stod(data[RZ])));

  return true;
}

}  // namespace lidar_align
