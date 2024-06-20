// Copyright 2020 Tier IV, Inc.
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

#include "multi_object_tracker/data_association/data_association.hpp"

#include "multi_object_tracker/data_association/solver/gnn_solver.hpp"
#include "multi_object_tracker/utils/utils.hpp"
#include "object_recognition_utils/object_recognition_utils.hpp"

#include <algorithm>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
double getMahalanobisDistance(
  const geometry_msgs::msg::Point & measurement, const geometry_msgs::msg::Point & tracker,
  const Eigen::Matrix2d & covariance)
{
  Eigen::Vector2d measurement_point;
  measurement_point << measurement.x, measurement.y;
  Eigen::Vector2d tracker_point;
  tracker_point << tracker.x, tracker.y;
  Eigen::MatrixXd mahalanobis_squared = (measurement_point - tracker_point).transpose() *
                                        covariance.inverse() * (measurement_point - tracker_point);
  return std::sqrt(mahalanobis_squared(0));
}

Eigen::Matrix2d getXYCovariance(const geometry_msgs::msg::PoseWithCovariance & pose_covariance)
{
  Eigen::Matrix2d covariance;
  covariance << pose_covariance.covariance[0], pose_covariance.covariance[1],
    pose_covariance.covariance[6], pose_covariance.covariance[7];
  return covariance;
}

double getFormedYawAngle(
  const geometry_msgs::msg::Quaternion & measurement_quat,
  const geometry_msgs::msg::Quaternion & tracker_quat, const bool distinguish_front_or_back = true)
{
  const double measurement_yaw =
    autoware::universe_utils::normalizeRadian(tf2::getYaw(measurement_quat));
  const double tracker_yaw = autoware::universe_utils::normalizeRadian(tf2::getYaw(tracker_quat));
  const double angle_range = distinguish_front_or_back ? M_PI : M_PI_2;
  const double angle_step = distinguish_front_or_back ? 2.0 * M_PI : M_PI;
  // Fixed measurement_yaw to be in the range of +-90 or 180 degrees of X_t(IDX::YAW)
  double measurement_fixed_yaw = measurement_yaw;
  while (angle_range <= tracker_yaw - measurement_fixed_yaw) {
    measurement_fixed_yaw = measurement_fixed_yaw + angle_step;
  }
  while (angle_range <= measurement_fixed_yaw - tracker_yaw) {
    measurement_fixed_yaw = measurement_fixed_yaw - angle_step;
  }
  return std::fabs(measurement_fixed_yaw - tracker_yaw);
}
}  // namespace

DataAssociation::DataAssociation(
  std::vector<int> can_assign_vector, std::vector<double> max_dist_vector,
  std::vector<double> max_area_vector, std::vector<double> min_area_vector,
  std::vector<double> max_rad_vector, std::vector<double> min_iou_vector)
: score_threshold_(0.01)
{
  {
    const int assign_label_num = static_cast<int>(std::sqrt(can_assign_vector.size()));
    Eigen::Map<Eigen::MatrixXi> can_assign_matrix_tmp(
      can_assign_vector.data(), assign_label_num, assign_label_num);
    can_assign_matrix_ = can_assign_matrix_tmp.transpose();
  }
  {
    const int max_dist_label_num = static_cast<int>(std::sqrt(max_dist_vector.size()));
    Eigen::Map<Eigen::MatrixXd> max_dist_matrix_tmp(
      max_dist_vector.data(), max_dist_label_num, max_dist_label_num);
    max_dist_matrix_ = max_dist_matrix_tmp.transpose();
  }
  {
    const int max_area_label_num = static_cast<int>(std::sqrt(max_area_vector.size()));
    Eigen::Map<Eigen::MatrixXd> max_area_matrix_tmp(
      max_area_vector.data(), max_area_label_num, max_area_label_num);
    max_area_matrix_ = max_area_matrix_tmp.transpose();
  }
  {
    const int min_area_label_num = static_cast<int>(std::sqrt(min_area_vector.size()));
    Eigen::Map<Eigen::MatrixXd> min_area_matrix_tmp(
      min_area_vector.data(), min_area_label_num, min_area_label_num);
    min_area_matrix_ = min_area_matrix_tmp.transpose();
  }
  {
    const int max_rad_label_num = static_cast<int>(std::sqrt(max_rad_vector.size()));
    Eigen::Map<Eigen::MatrixXd> max_rad_matrix_tmp(
      max_rad_vector.data(), max_rad_label_num, max_rad_label_num);
    max_rad_matrix_ = max_rad_matrix_tmp.transpose();
  }
  {
    const int min_iou_label_num = static_cast<int>(std::sqrt(min_iou_vector.size()));
    Eigen::Map<Eigen::MatrixXd> min_iou_matrix_tmp(
      min_iou_vector.data(), min_iou_label_num, min_iou_label_num);
    min_iou_matrix_ = min_iou_matrix_tmp.transpose();
  }

  gnn_solver_ptr_ = std::make_unique<gnn_solver::MuSSP>();
}

void DataAssociation::assign(
  const Eigen::MatrixXd & src, std::unordered_map<int, int> & direct_assignment,
  std::unordered_map<int, int> & reverse_assignment)
{
  std::vector<std::vector<double>> score(src.rows());
  for (int row = 0; row < src.rows(); ++row) {
    score.at(row).resize(src.cols());
    for (int col = 0; col < src.cols(); ++col) {
      score.at(row).at(col) = src(row, col);
    }
  }
  // Solve
  gnn_solver_ptr_->maximizeLinearAssignment(score, &direct_assignment, &reverse_assignment);

  for (auto itr = direct_assignment.begin(); itr != direct_assignment.end();) {
    if (src(itr->first, itr->second) < score_threshold_) {
      itr = direct_assignment.erase(itr);
      continue;
    } else {
      ++itr;
    }
  }
  for (auto itr = reverse_assignment.begin(); itr != reverse_assignment.end();) {
    if (src(itr->second, itr->first) < score_threshold_) {
      itr = reverse_assignment.erase(itr);
      continue;
    } else {
      ++itr;
    }
  }
}

Eigen::MatrixXd DataAssociation::calcScoreMatrix(
  const autoware_perception_msgs::msg::DetectedObjects & measurements,
  const std::list<std::shared_ptr<Tracker>> & trackers)
{
  Eigen::MatrixXd score_matrix =
    Eigen::MatrixXd::Zero(trackers.size(), measurements.objects.size());
  size_t tracker_idx = 0;
  for (auto tracker_itr = trackers.begin(); tracker_itr != trackers.end();
       ++tracker_itr, ++tracker_idx) {
    const std::uint8_t tracker_label = (*tracker_itr)->getHighestProbLabel();

    for (size_t measurement_idx = 0; measurement_idx < measurements.objects.size();
         ++measurement_idx) {
      const autoware_perception_msgs::msg::DetectedObject & measurement_object =
        measurements.objects.at(measurement_idx);
      const std::uint8_t measurement_label =
        object_recognition_utils::getHighestProbLabel(measurement_object.classification);

      double score = 0.0;
      if (can_assign_matrix_(tracker_label, measurement_label)) {
        autoware_perception_msgs::msg::TrackedObject tracked_object;
        (*tracker_itr)->getTrackedObject(measurements.header.stamp, tracked_object);

        const double max_dist = max_dist_matrix_(tracker_label, measurement_label);
        const double dist = autoware::universe_utils::calcDistance2d(
          measurement_object.kinematics.pose_with_covariance.pose.position,
          tracked_object.kinematics.pose_with_covariance.pose.position);

        bool passed_gate = true;
        // dist gate
        if (passed_gate) {
          if (max_dist < dist) passed_gate = false;
        }
        // area gate
        if (passed_gate) {
          const double max_area = max_area_matrix_(tracker_label, measurement_label);
          const double min_area = min_area_matrix_(tracker_label, measurement_label);
          const double area = autoware::universe_utils::getArea(measurement_object.shape);
          if (area < min_area || max_area < area) passed_gate = false;
        }
        // angle gate
        if (passed_gate) {
          const double max_rad = max_rad_matrix_(tracker_label, measurement_label);
          const double angle = getFormedYawAngle(
            measurement_object.kinematics.pose_with_covariance.pose.orientation,
            tracked_object.kinematics.pose_with_covariance.pose.orientation, false);
          if (std::fabs(max_rad) < M_PI && std::fabs(max_rad) < std::fabs(angle))
            passed_gate = false;
        }
        // mahalanobis dist gate
        if (passed_gate) {
          const double mahalanobis_dist = getMahalanobisDistance(
            measurement_object.kinematics.pose_with_covariance.pose.position,
            tracked_object.kinematics.pose_with_covariance.pose.position,
            getXYCovariance(tracked_object.kinematics.pose_with_covariance));
          if (3.035 /*99%*/ <= mahalanobis_dist) passed_gate = false;
        }
        // 2d iou gate
        if (passed_gate) {
          const double min_iou = min_iou_matrix_(tracker_label, measurement_label);
          const double min_union_iou_area = 1e-2;
          const double iou = object_recognition_utils::get2dIoU(
            measurement_object, tracked_object, min_union_iou_area);
          if (iou < min_iou) passed_gate = false;
        }

        // all gate is passed
        if (passed_gate) {
          score = (max_dist - std::min(dist, max_dist)) / max_dist;
          if (score < score_threshold_) score = 0.0;
        }
      }
      score_matrix(tracker_idx, measurement_idx) = score;
    }
  }

  return score_matrix;
}
