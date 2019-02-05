#include <kitti_ros/perception/sensor_fusion.h>

SensorFusion::SensorFusion() {
    nh_ = ros::NodeHandlePtr(new ros::NodeHandle());

    // Publish rgb colored pointcloud
    rgb_pointcloud_pub_ =
        nh_->advertise<sensor_msgs::PointCloud2>("kitti_rgb_pointcloud", 1);

    // Publish segmented pointcloud from maskrcnn detection image;

    segmented_pointcloud_from_maskrcnn_pub_ =
        nh_->advertise<sensor_msgs::PointCloud2>(
            "segmented_pointcloud_from_maskrcnn", 1);

    // publish point cloud projected IMage
    pointcloud_projected_image_pub_ =
        nh_->advertise<sensor_msgs::Image>("pointcloud_projected_image", 1);

    // publish Kitti raw point cloud
    kitti_pcl_pub_ =
        nh_->advertise<sensor_msgs::PointCloud2>("kitti_raw_pointcloud", 1);
    // publish kitti raw image
    kitti_image_pub_ = nh_->advertise<sensor_msgs::Image>("kitti_raw_image", 1);

    // publish birdview pointcloud Image
    birdview_pointcloud_image_pub_ =
        nh_->advertise<sensor_msgs::Image>("image_from_colorful_PCL", 1);
}

SensorFusion::~SensorFusion() {}

void SensorFusion::SetKITTIDataOperator(KITTIDataOperator* value) {
    kitti_data_operator_ = value;
}

const KITTIDataOperator* SensorFusion::GetKITTIDataOperator() {
    return kitti_data_operator_;
}

void SensorFusion::SetKittiObjectOperator(KittiObjectOperator* value) {
    kitti_object_operator_ = value;
}

const KittiObjectOperator* SensorFusion::GetKittiObjectOperator() {
    return kitti_object_operator_;
}

void SensorFusion::SetSegmentedLidarScan(sensor_msgs::PointCloud2 value) {
    segmented_lidar_scan_ = value;
}

sensor_msgs::PointCloud2 SensorFusion::GetSegmentedLidarScan() {
    return segmented_lidar_scan_;
}

void SensorFusion::SetTools(Tools* value) { tools_ = value; }

const Tools* SensorFusion::GetTools() { return tools_; }

void SensorFusion::FillKittiData4Fusion() {
    lidar_scan_ = kitti_data_operator_->GetLidarScan();
    lidar_scan_.header.stamp = ros::Time::now();
    lidar_scan_.header.frame_id = "camera_link";

    kitti_left_cam_img_ = kitti_data_operator_->GetCameraImage();
}

void SensorFusion::ProcessFusion(std::string training_image_name) {
    pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud(
        new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr rgb_out_cloud(
        new pcl::PointCloud<pcl::PointXYZRGB>);

    cv::Mat cv_pointcloud_projected_image = kitti_left_cam_img_.clone();

    pcl::fromROSMsg(lidar_scan_, *in_cloud);

    Eigen::MatrixXf matrix_velodyne_points =
        MatrixXf::Zero(4, in_cloud->size());

    for (int i = 0; i < in_cloud->size(); ++i) {
        matrix_velodyne_points(0, i) = in_cloud->points[i].x;
        matrix_velodyne_points(1, i) = in_cloud->points[i].y;
        matrix_velodyne_points(2, i) = in_cloud->points[i].z;
        matrix_velodyne_points(3, i) = 1;
    }

    Eigen::MatrixXf matrix_image_points =
        tools_->transformCamToRectCam(matrix_velodyne_points);
    matrix_image_points = tools_->transformRectCamToImage(matrix_image_points);

    for (int m = 0; m < matrix_image_points.cols(); m++) {
        cv::Point point;
        point.x = matrix_image_points(0, m);
        point.y = matrix_image_points(1, m);

        // Store korners in pixels only of they are on image plane
        if (point.x >= 0 && point.x <= 1242) {
            if (point.y >= 0 && point.y <= 375) {
                pcl::PointXYZRGB colored_3d_point;

                cv::Vec3b rgb_pixel =
                    kitti_left_cam_img_.at<cv::Vec3b>(point.y, point.x);

                colored_3d_point.x = matrix_velodyne_points(0, m);
                colored_3d_point.y = matrix_velodyne_points(1, m);
                colored_3d_point.z = matrix_velodyne_points(2, m);

                colored_3d_point.r = rgb_pixel[2];
                colored_3d_point.g = rgb_pixel[1];
                colored_3d_point.b = rgb_pixel[0];

                if (colored_3d_point.z > 0) {
                    float distance_to_point =
                        SensorFusion::EuclidianDistofPoint(&colored_3d_point);

                    cv::circle(cv_pointcloud_projected_image, point, 1,
                               cv::Scalar(120, 0, distance_to_point * 15), 1);

                    rgb_out_cloud->points.push_back(colored_3d_point);
                }
            }
        }
    }

    // Create birdeyeview Lidar Image , with rgb values taken from corresponding
    // pixel
    rgb_out_cloud->width = 1;
    rgb_out_cloud->height = rgb_out_cloud->points.size();
    SensorFusion::CreateBirdviewPointcloudImage(rgb_out_cloud,
                                                training_image_name);

    // Publish raw point cloud and raw rgb image
    SensorFusion::PublishRawData();

    // prepare and publish RGB colored Lidar scan

    sensor_msgs::PointCloud2 cloud_msg;
    pcl::toROSMsg(*rgb_out_cloud, cloud_msg);
    cloud_msg.header = lidar_scan_.header;
    rgb_pointcloud_pub_.publish(cloud_msg);

    // Prepare and publish Point cloud projected kitti image
    cv_bridge::CvImage pointcloud_projected_image;
    pointcloud_projected_image.image = cv_pointcloud_projected_image;
    pointcloud_projected_image.encoding = "bgr8";
    pointcloud_projected_image.header.stamp = ros::Time::now();
    pointcloud_projected_image_pub_.publish(
        pointcloud_projected_image.toImageMsg());
}

void SensorFusion::PublishRawData() {
    // Prepare and publish KITTI raw image
    cv_bridge::CvImage cv_kitti_image;
    cv_kitti_image.image = kitti_left_cam_img_;
    cv_kitti_image.encoding = "bgr8";
    cv_kitti_image.header.stamp = ros::Time::now();
    kitti_image_pub_.publish(cv_kitti_image.toImageMsg());

    // PUBLISH Raw Lidar scan
    kitti_pcl_pub_.publish(lidar_scan_);
}

float SensorFusion::EuclidianDistofPoint(pcl::PointXYZRGB* colored_3d_point) {
    float distance = std::sqrt(std::pow(colored_3d_point->x, 2) +
                               std::pow(colored_3d_point->y, 2) +
                               std::pow(colored_3d_point->z, 2));
    return distance;
}

void SensorFusion::CreateBirdviewPointcloudImage(
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr out_cloud, std::string image_file) {
    cv::Mat bird_view_image(1200, 1200, CV_8UC3);
    bird_view_image.setTo(cv::Scalar(255, 255, 255));

    for (int r = 0; r < out_cloud->points.size(); r++) {
        pcl::PointXYZRGB rgb_point = out_cloud->points[r];

        if (rgb_point.z < 60 && rgb_point.x > -30 && rgb_point.x < 30) {
            cv::Point point;

            rgb_point.z -= 60;
            rgb_point.x += 30;

            point.x = rgb_point.x * 20;
            point.y = -rgb_point.z * 20;

            cv::Vec3b rgb_pixel, dense;

            rgb_pixel[2] = rgb_point.r;
            rgb_pixel[1] = rgb_point.g;
            rgb_pixel[0] = rgb_point.b;

            dense[2] = 255;
            dense[1] = 0;
            dense[0] = 0;

            if (point.x > 0 && point.x < 1200) {
                if (point.y > 0 && point.y < 1200) {
                    bird_view_image.at<cv::Vec3b>(point.y, point.x) = rgb_pixel;
                }
            }
        }
    }

    // Prepare and publish colorful image from PCL
    cv_bridge::CvImage cv_colorful_image_from_PCL;
    cv_colorful_image_from_PCL.image = bird_view_image;
    cv_colorful_image_from_PCL.encoding = "bgr8";
    cv_colorful_image_from_PCL.header.stamp = ros::Time::now();
    birdview_pointcloud_image_pub_.publish(
        cv_colorful_image_from_PCL.toImageMsg());

    cv::imwrite(image_file, bird_view_image);

    // pcl::io::savePCDFileASCII("test_pcd.pcd", *out_cloud);
}

void SensorFusion::ProcessLabelofBEVImage(std::string& label_infile_string,
                                          std::string image_file_path) {
    std::ifstream label_infile(label_infile_string.c_str());

    KittiObjectOperator::KittiObjectsThisFrame kitti_objects =
        kitti_object_operator_->GetAllKittiObjectsFrame(label_infile);

    cv::Mat BEV_image = cv::imread(image_file_path, cv::IMREAD_COLOR);

    for (int k = 0; k < kitti_objects.size(); k++) {
        if (kitti_objects[k].type == "Car") {
            std::vector<float> dimensions, position;

            dimensions =
                kitti_object_operator_->dimensionsVector(kitti_objects[k]);
            position = kitti_object_operator_->positionVector(kitti_objects[k]);

            Eigen::MatrixXd corners(3, 8);

            std::cout << "KORNER FROM LABEL " << corners << std::endl;

            corners = kitti_ros_util::ComputeCorners(
                dimensions, position, kitti_objects[k].rotation_y);

            cv::Point pt1_on2D, pt2_on2D, pt3_on2D, pt4_on2D, center;

            pt1_on2D.x = corners(0, 0);
            pt1_on2D.y = corners(2, 0);

            pt2_on2D.x = corners(0, 1);
            pt2_on2D.y = corners(2, 1);

            pt3_on2D.x = corners(0, 2);
            pt3_on2D.y = corners(2, 2);

            pt4_on2D.x = corners(0, 3);
            pt4_on2D.y = corners(2, 3);

            pt1_on2D.x += 30;
            pt1_on2D.y -= 60;

            pt2_on2D.x += 30;
            pt2_on2D.y -= 60;

            pt3_on2D.x += 30;
            pt3_on2D.y -= 60;

            pt4_on2D.x += 30;
            pt4_on2D.y -= 60;

            // scale up to image dimensions 1200 x 900

            pt1_on2D.x *= 20;
            pt2_on2D.x *= 20;
            pt3_on2D.x *= 20;
            pt4_on2D.x *= 20;

            pt1_on2D.y *= -20;
            pt2_on2D.y *= -20;
            pt3_on2D.y *= -20;
            pt4_on2D.y *= -20;

            center.x = position[0];
            center.y = position[2];
            center.x += 30;
            center.y -= 60;
            center.x *= 20;
            center.y *= -20;

            // cv::rectangle(frame, rect_on2D, c, 2, 2, 0);
            cv::Scalar clr = cv::Scalar(0, 0, 255);

            cv::circle(BEV_image, center, 4, clr, 1, 1, 0);
            cv::line(BEV_image, pt1_on2D, pt2_on2D, clr, 1, 8);
            cv::line(BEV_image, pt2_on2D, pt3_on2D, clr, 1, 8);
            cv::line(BEV_image, pt3_on2D, pt4_on2D, clr, 1, 8);
            cv::line(BEV_image, pt4_on2D, pt1_on2D, clr, 1, 8);
        }
    }
    cv::imwrite(image_file_path, BEV_image);
}

void SensorFusion::SegmentedPointCloudFromMaskRCNN(
    cv::Mat* maskrcnn_segmented_image) {
    pcl::PointCloud<pcl::PointXYZ>::Ptr in_cloud(
        new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr rgb_out_cloud(
        new pcl::PointCloud<pcl::PointXYZRGB>);

    pcl::PointCloud<pcl::PointXYZ>::Ptr out_cloud(
        new pcl::PointCloud<pcl::PointXYZ>);

    pcl::fromROSMsg(lidar_scan_, *in_cloud);

    Eigen::MatrixXf matrix_velodyne_points =
        MatrixXf::Zero(4, in_cloud->size());

    for (int i = 0; i < in_cloud->size(); ++i) {
        matrix_velodyne_points(0, i) = in_cloud->points[i].x;
        matrix_velodyne_points(1, i) = in_cloud->points[i].y;
        matrix_velodyne_points(2, i) = in_cloud->points[i].z;
        matrix_velodyne_points(3, i) = 1;
    }

    Eigen::MatrixXf matrix_image_points =
        tools_->transformCamToRectCam(matrix_velodyne_points);
    matrix_image_points = tools_->transformRectCamToImage(matrix_image_points);

    for (int m = 0; m < matrix_image_points.cols(); m++) {
        cv::Point point;
        point.x = matrix_image_points(0, m);
        point.y = matrix_image_points(1, m);

        // Store korners in pixels only of they are on image plane
        if (point.x >= 0 && point.x <= 1242) {
            if (point.y >= 0 && point.y <= 375) {
                pcl::PointXYZRGB colored_3d_point;
                pcl::PointXYZ out_cloud_point;

                cv::Vec3b rgb_pixel =
                    maskrcnn_segmented_image->at<cv::Vec3b>(point.y, point.x);

                colored_3d_point.x = matrix_velodyne_points(0, m);
                colored_3d_point.y = matrix_velodyne_points(1, m);
                colored_3d_point.z = matrix_velodyne_points(2, m);

                colored_3d_point.r = rgb_pixel[2];
                colored_3d_point.g = rgb_pixel[1];
                colored_3d_point.b = rgb_pixel[0];

                if (rgb_pixel[2] != 255 && rgb_pixel[1] != 255 &&
                    rgb_pixel[0] != 255 && colored_3d_point.z > 0 &&
                    colored_3d_point.y < 1.65) {
                    rgb_out_cloud->points.push_back(colored_3d_point);
                }
            }
        }
    }

    // Create the filtering object
    pcl::StatisticalOutlierRemoval<pcl::PointXYZRGB> sor;
    sor.setInputCloud(rgb_out_cloud);
    sor.setMeanK(12);
    sor.setStddevMulThresh(0.2);
    sor.filter(*rgb_out_cloud);

    pcl::RadiusOutlierRemoval<pcl::PointXYZRGB> outrem;
    // build the filter
    outrem.setInputCloud(rgb_out_cloud);
    outrem.setRadiusSearch(0.3);
    outrem.setMinNeighborsInRadius(4);
    // apply filter
    outrem.filter(*rgb_out_cloud);

    // prepare and publish RGB colored Lidar scan
    sensor_msgs::PointCloud2 maskrcnn_cloud_msg;
    pcl::toROSMsg(*rgb_out_cloud, maskrcnn_cloud_msg);
    maskrcnn_cloud_msg.header = lidar_scan_.header;
    segmented_pointcloud_from_maskrcnn_pub_.publish(maskrcnn_cloud_msg);

    SensorFusion::SetSegmentedLidarScan(maskrcnn_cloud_msg);
}