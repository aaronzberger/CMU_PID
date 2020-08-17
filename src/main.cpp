#include <numeric>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include "ros/ros.h"
#include "nav_msgs/Odometry.h"
#include "rosgraph_msgs/Clock.h"
#include "geometry_msgs/Twist.h"
#include "gazebo_msgs/ModelStates.h"
#include "visualization_msgs/Marker.h"
#include <pcl_ros/point_cloud.h>
#include <pcl/conversions.h>
#include <pcl/point_types.h>
#include <tf/transform_datatypes.h>
#include "pid.h"
#include "pid.cpp"
#include <opencv2/core/mat.hpp>
#include "opencv2/imgproc.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/imgcodecs.hpp"
#include <png++/png.hpp>

// USER PARAMETERS
constexpr double distanceBetweenRows = 3.0;
constexpr double inlierThreshold = 0.39;
constexpr unsigned numRansacLoops = 250;
constexpr double turningSpeed = 0.4;
constexpr double drivingSpeed = 0.2;

typedef pcl::PointCloud<pcl::PointXYZ> PointCloud;

struct line {
    double a, b, c, distance, theta;
    unsigned inliers;
};

struct lineGroup {
    std::vector<line> lines;
    int totalInliers;
};

//For the world two_walls_3m.world, true lines are below:
line left;
line middle;
line right;


// Function Prototypes
void clockCallback(const rosgraph_msgs::Clock::ConstPtr &msg);
bool isGroundPoint(const pcl::PointXYZ &pt);
void odomCallback(const nav_msgs::Odometry::ConstPtr &msg);
void velodyneCallBack(const PointCloud::ConstPtr &pcl);
void displayLine(line line, float r, float g, float b, int id);
lineGroup ransac(const PointCloud::ConstPtr &pcl);
bool close(line line1, line line2);
double lineToPtDistance(double x, double y, line l);
double lineToPtAngle(double x, double y, line l);
double getSlope(line line);
double getYInt(line line);
void printPointCloud(const PointCloud::ConstPtr &pcl);
template <typename T> void moveVecBack(std::vector<T> &vec);

double currentTime, yaw, x, y, lastMotionUpdate;
ros::Publisher velPub, markerPub;
PID controller;
int endOfRowCounter;
int startOfRowCounter;
bool turning;
std::ofstream testing1;

//Storing the past number of valid points for a moving average
std::vector<int> numAheadPtsVec(49, -1);
std::vector<double> lineTrackerFilter(5, -99);

/**
 * @brief Updates the current time variable
 * 
 * @param msg the message received from the clock ros topic
 */
void clockCallback(const rosgraph_msgs::Clock::ConstPtr &msg) {
    currentTime = (msg->clock.sec * 1000) + int(msg->clock.nsec / 1e6);
}

/**
 * @brief Whether the point is a point not belonging to a wall
 * 
 * @param pt the point in the point cloud, containing an X, Y, and Z
 * @return a boolean representing whether the point is not on a wall
 */
bool isGroundPoint(const pcl::PointXYZ &pt) {
    return (pt.z > -0.375 && pt.z < -0.365);
}

/**
 * @brief Update the yaw variable
 * 
 * @param msg the message received from the odometry ros topic
 */
void odomCallback(const nav_msgs::Odometry::ConstPtr &msg) {
    // x = msg->pose.pose.position.x;
    // y = msg->pose.pose.position.y;

    // tf::Quaternion quat;
    // quat.setW(msg->pose.pose.orientation.w);
    // quat.setX(msg->pose.pose.orientation.x);
    // quat.setY(msg->pose.pose.orientation.y);
    // quat.setZ(msg->pose.pose.orientation.z);

    // yaw = tf::getYaw(quat);
}

void modelStateCallback(const gazebo_msgs::ModelStates::ConstPtr &msg) {
    unsigned idx {0};
    for(int i{0}; i < msg->name.size(); i++) {
        if(msg->name.at(i) == "/") idx = i;
    }
    x = {msg->pose.at(idx).position.x};
    y = {msg->pose.at(idx).position.y};
    
    tf::Quaternion quat;
    quat.setW(msg->pose.at(idx).orientation.w);
    quat.setX(msg->pose.at(idx).orientation.x);
    quat.setY(msg->pose.at(idx).orientation.y);
    quat.setZ(msg->pose.at(idx).orientation.z);

    yaw = tf::getYaw(quat);
}

/**
 * @brief Move items in a vector back one index
 * 
 * @param vec the vector
 */
template <typename T> void moveVecBack(std::vector<T> &vec) {
    int numPts = vec.size();
    for(int i{1}; i < numPts; i++) {
        vec.at(i - 1) = vec.at(i);
    }
}

/**
 * @brief display a line in the rviz simulator
 * 
 * @param line the line to display
 * @param r the r value in rgb color scheme
 * @param g the g value in rgb color scheme
 * @param b the b value in rgb color scheme
 * @param id the ID to assign to the line
 */
void displayLine(line line, float r, float g, float b, int id) {
    uint32_t shape = visualization_msgs::Marker::LINE_STRIP;

    visualization_msgs::Marker marker;
    marker.header.frame_id = "odom";
    marker.header.stamp = ros::Time::now();

    marker.ns = "shapes";
    marker.id = id;
    marker.type = shape;
    marker.action = visualization_msgs::Marker::ADD;

    // Calculate two points from which to build the line
    geometry_msgs::Point pt1;
    pt1.x = -100;
    pt1.y = ((-(line.a / line.b)) * pt1.x) - (line.c / line.b);
    pt1.z = 0;

    pt1.x += x;
    pt1.y += y;

    geometry_msgs::Point pt2;
    pt2.x = 100;
    pt2.y = ((-(line.a / line.b)) * pt2.x) - (line.c / line.b);
    pt2.z = 0;

    pt2.x += x;
    pt2.y += y;

    marker.points.push_back(pt1);
    marker.points.push_back(pt2);

    // Set the thickness of the line (0-1)
    marker.scale.x = 0.15;

    // Set the color of the line
    marker.color.r = r;
    marker.color.g = g;
    marker.color.b = b;
    marker.color.a = 1.0;

    marker.lifetime = ros::Duration();
    markerPub.publish(marker);
}

/**
 * @brief Calculate the slope of a line in standard form
 * 
 * @param line the line
 * @return the slope of the line
 */
double getSlope(line line) {
    return -(line.a / line.b);
}

/**
 * @brief Calculate the Y-intercept of a line in standard form
 * 
 * @param line the line
 * @return the y-intercept of the line
 */
double getYInt(line line) {
    return -(line.c / line.b);
}

/**
 * @brief Determine if two lines are too close to be different lines
 * 
 * @param line1 the first line
 * @param line2 the second line
 * 
 * @return whether the two lines represent the same detected line
 */
bool close(line line1, line line2) {
    if(std::abs(-(line1.a/line1.b) - (-(line2.a/line2.b))) < 0.2) {
        return (std::abs((line1.c/line1.b) - (line2.c/line2.b)) < 1.0);
    }
    return false;
}

/**
 * @brief Determine the distance from a point to a line in standard form
 * 
 * @param x the x of the point
 * @param y the y of the point
 * @param line the line
 * 
 * @return the distance from the point to the line in standard form
 */
double lineToPtDistance(double x, double y, line l) {
    return (std::abs((l.a * x) + (l.b * y) + l.c) / (std::sqrt(l.a*l.a + l.b*l.b)));
}

/**
 * @brief Determine the angle from a point to a line
 * 
 * @param x the x of the point
 * @param y the y of the point
 * @param line the line
 * 
 * @return the angle from the point to the line (parallel means 90)
 */
double lineToPtAngle(double x, double y, line l) {
    return M_PI_2 + std::atan(getSlope(l));
}

/**
 * @brief Print the contents of a point cloud for testing
 * 
 * @param pcl the point cloud to print
 */
void printPointCloud(const PointCloud::ConstPtr &pcl) {
    for(int i{0}; i < pcl->width; i++) {
        if(!isGroundPoint(pcl->points.at(i))) {
            std::cout << "X: " << pcl->points.at(i).x << " Y: " << pcl->points.at(i).y << " Z: " << pcl->points.at(i).z << std::endl;
        }
    }
}

/**
 * @brief Update the output to the robot using Lidar data, through a PID controller
 * 
 * @param pcl the message received from the Lidar ros topic (velodyne)
 */
void velodyneCallBack(const PointCloud::ConstPtr &pcl) {
    if(currentTime - lastMotionUpdate > 10) {
        // Collect the detected lines from the RANSAC algorithm
        lineGroup lines {ransac(pcl)};

        // Output variables for Kalman testing
        std::cout << "True Distance: " << lineToPtDistance(x, y, middle) << " True Angle: " << M_PI_2 - yaw << std::endl;
        std::cout << "Distance: " << lines.lines.at(0).distance << " Angle: " << lines.lines.at(0).theta << std::endl;
        std::cout << "X: " << x << " Y: " << y << " T: " << yaw << std::endl;
        std::cout << "-------------------" << std::endl;

        // DETERMINE TURNING OR DRIVING
        //***********************************************//

        testing1 << x << "," << y << "," << yaw << "," << lines.lines.at(0).distance << "," << lines.lines.at(0).theta << "\n";

        // Determine the ratio of points ahead of the robot
        int numValidPts {0};
        int numAheadPts {0};
        for(int i{0}; i < pcl->width; i++) {
            if(!isGroundPoint(pcl->points.at(i))) {
                numValidPts++;
                if(pcl->points.at(i).x > 0) numAheadPts++;
            }
        }
        double aheadRatio {static_cast<double>(numAheadPts) / numValidPts};

        // Decide if we should switch modes
        if(!turning) {
            if(aheadRatio < 0.2) endOfRowCounter++;
            if(endOfRowCounter > 20) {
                turning = true;
                endOfRowCounter = 0;
            }
        } else {
            if(aheadRatio > 0.75) startOfRowCounter++;
            if(startOfRowCounter > 20) {
                turning = false;
                startOfRowCounter = 0;
            }
        }
        //***********************************************//

        if(turning) {
            // Construct a velocity message to give to the Husky based on basic constant angular velocity turning
            geometry_msgs::Twist twist;

            twist.linear.x = turningSpeed;
            twist.linear.y = 0;
            twist.linear.z = 0;

            twist.angular.x = 0;
            twist.angular.y = 0;
            twist.angular.z = twist.linear.x / (distanceBetweenRows / 2);

            velPub.publish(twist);
            lastMotionUpdate = currentTime;
        } else if(!(lines.lines.size() == 0)) {
            // Use a moving average filter for the Y-intercept of the tracking line for de-noising
            moveVecBack<double>(lineTrackerFilter);
            lineTrackerFilter.at(lineTrackerFilter.size() - 1) = getYInt(lines.lines.at(2));

            double yIntAve {0};
            if(!(lineTrackerFilter.at(0) == -99)) {
                for(auto val : lineTrackerFilter) yIntAve += val;
                yIntAve /= lineTrackerFilter.size();
            }

            // Display the originally detected line in blue
            displayLine(lines.lines.at(0), 0, 0, 1, 2);

            //Run the PID controller
            double angVel {controller.calculate(yIntAve, currentTime)};

            //Construct a velocity message to give to the Husky
            geometry_msgs::Twist twist;

            twist.linear.x = drivingSpeed;
            twist.linear.y = 0;
            twist.linear.z = 0;

            twist.angular.x = 0;
            twist.angular.y = 0;
            twist.angular.z = angVel;

            //velPub.publish(twist);
            lastMotionUpdate = currentTime;
        } else {
            std::cout << "Size " << lines.lines.size() << " Inliers " << lines.totalInliers << std::endl;
            std::cout << "NO LINE GROUP FOUND" << std::endl;
        }
    }
}

/**
 * @brief Perform a RANSAC line detection on an XYZ Point Cloud
 * 
 * @param pcl the point cloud received from the Lidar (vlp16)
 * @return the group of lines that the line detection yielded
 */
lineGroup ransac(const PointCloud::ConstPtr &pcl) {
    lineGroup bestLineGroup;
    bestLineGroup.totalInliers = 0;

    line placeholder;
    placeholder.inliers = 0;
    bestLineGroup.lines.push_back(placeholder);

    for(int i{0}; i < numRansacLoops; i++) {
        // Find two random points for RANSAC line detection
        int pt1Idx {std::rand() / ((RAND_MAX) / (pcl->width - 1))};
        while(isGroundPoint(pcl->points.at(pt1Idx))) {
            pt1Idx = std::rand() / ((RAND_MAX) / (pcl->width - 1));
        }
        int pt2Idx {std::rand() / ((RAND_MAX) / (pcl->width - 1))};
        while(isGroundPoint(pcl->points.at(pt2Idx)) || pt2Idx == pt1Idx) {
            pt2Idx = std::rand() / ((RAND_MAX) / (pcl->width - 1));
        }

        // Construct a line from those two random points
        line currentLine;
        currentLine.a = pcl->points.at(pt2Idx).y - pcl->points.at(pt1Idx).y;
        currentLine.b = pcl->points.at(pt1Idx).x - pcl->points.at(pt2Idx).x;
        currentLine.c = -((currentLine.a * pcl->points.at(pt1Idx).x) + (currentLine.b * pcl->points.at(pt1Idx).y));

        // Determine the number of inliers in the originally detected line
        currentLine.inliers = 0;
        for(int j{0}; j < pcl->width; j++) {
            if(!isGroundPoint(pcl->points.at(j))) {
                double distance {lineToPtDistance(pcl->points.at(j).x, pcl->points.at(j).y, currentLine)};
                if(std::abs(distance) <= inlierThreshold) currentLine.inliers++;
            }
        }

        // Determine the current distance to the line
        currentLine.distance = lineToPtDistance(0, 0, currentLine);
        currentLine.theta = lineToPtAngle(0, 0, currentLine);

        if(getYInt(currentLine) > 0) {
            // Construct a contestant line group from the original line
            lineGroup currentLineGroup;
            currentLineGroup.lines.push_back(currentLine);
            currentLineGroup.totalInliers = currentLine.inliers;

            // Construct a complementary line that is parallel to the original line and add it to the line group
            line rightLine;
            rightLine.a = currentLine.a;
            rightLine.b = currentLine.b;
            rightLine.c = -(distanceBetweenRows * currentLine.b) + currentLine.c;

            rightLine.inliers = 0;
            for(int j{0}; j < pcl->width; j++) {
                if(!isGroundPoint(pcl->points.at(j))) {
                    double distance {lineToPtDistance(pcl->points.at(j).x, pcl->points.at(j).y, rightLine)};
                    if(std::abs(distance) <= inlierThreshold) rightLine.inliers++;
                }
            }

            // Determine the current distance to the line
            rightLine.distance = lineToPtDistance(0, 0, rightLine);
            rightLine.theta = lineToPtAngle(0, 0, rightLine);

            currentLineGroup.lines.push_back(rightLine);
            //currentLineGroup.totalInliers += rightLine.inliers;

            // Determine if this is the best line group
            if(currentLineGroup.totalInliers >= bestLineGroup.totalInliers) bestLineGroup = currentLineGroup;
        }
    }

    // Construct a line in the middle of the other two lines as a tracker
    line tracker;
    tracker.a = bestLineGroup.lines.at(0).a;
    tracker.b = bestLineGroup.lines.at(0).b;
    tracker.c = (bestLineGroup.lines.at(0).c + bestLineGroup.lines.at(1).c) / 2;
    tracker.distance = lineToPtDistance(0, 0, tracker);
    tracker.theta = lineToPtAngle(0, 0, tracker);

    bestLineGroup.lines.push_back(tracker);
    return bestLineGroup;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "wall_controller");

    if(argc != 4) {
        std::cout << "Expected 3 arguments: P, I, D" << std::endl;
        return -1;  // by convention, return a non-zero code to indicate error
    }

    ros::NodeHandle n;

    //Initialize true state walls based on known positions in the simulator
    left.a = 0;
    left.b = 1;
    left.c = -4.5;

    middle.a = 0;
    middle.b = 1;
    middle.c = -1.5;

    right.a = 0;
    right.b = 1;
    right.c = 1.5;

    endOfRowCounter = 0;
    startOfRowCounter = 0;

    testing1 = std::ofstream("testing1.csv");

    std::srand(std::time(nullptr));

    ros::Subscriber subOdom = n.subscribe<nav_msgs::Odometry>("/odometry/filtered", 50, odomCallback);
    ros::Subscriber subModelStates = n.subscribe<gazebo_msgs::ModelStates>("/gazebo/model_states", 50, modelStateCallback);
    ros::Subscriber subClock = n.subscribe<rosgraph_msgs::Clock>("/clock", 50, clockCallback);
    ros::Subscriber subVelodyne = n.subscribe<PointCloud>("/velodyne_points", 50, velodyneCallBack);
    velPub = n.advertise<geometry_msgs::Twist>("/cmd_vel", 1);
    markerPub = n.advertise<visualization_msgs::Marker>("/visualization_marker", 1);

    controller = PID(std::atof(argv[1]), std::atof(argv[2]), std::atof(argv[3]));

    controller.setInverted(true);
    controller.setSetPoint(0);
    controller.setOutputLimits(-0.4, 0.4);
    controller.setMaxIOutput(0.2);

    ros::spin();

    testing1.close();

    return 0;
}