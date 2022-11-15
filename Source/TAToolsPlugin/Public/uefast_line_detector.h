// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

//#ifndef __OPENCV_FAST_LINE_DETECTOR_HPP__
//#define __OPENCV_FAST_LINE_DETECTOR_HPP__

#pragma once
#include "TAToolsPlugin.h"

//#ifndef Algorithm
//#define Algorithm
//class Algorithm {};
//#endif

using namespace cv;
//using namespace cv::ximgproc;

class UE_FLD : public cv::Algorithm
{
public:
    /** @example fld_lines.cpp
        An example using the FastLineDetector
        */
    /** @brief Finds lines in the input image.
        This is the output of the default parameters of the algorithm on the above
        shown image.

        ![image](pics/corridor_fld.jpg)

        @param _image A grayscale (CV_8UC1) input image. If only a roi needs to be
        selected, use: `fld_ptr-\>detect(image(roi), lines, ...);
        lines += Scalar(roi.x, roi.y, roi.x, roi.y);`
        @param _lines A vector of Vec4f elements specifying the beginning
        and ending point of a line.  Where Vec4f is (x1, y1, x2, y2), point
        1 is the start, point 2 - end. Returned lines are directed so that the
        brighter side is on their left.
        */
    CV_WRAP virtual void detect(InputArray _image, TArray<Vec4f>& _lines) = 0;

    /** @brief Draws the line segments on a given image.
        @param _image The image, where the lines will be drawn. Should be bigger
        or equal to the image, where the lines were found.
        @param lines A vector of the lines that needed to be drawn.
        @param draw_arrow If true, arrow heads will be drawn.
    */
    CV_WRAP virtual void drawSegments(InputOutputArray _image, InputArray lines,
            bool draw_arrow = false) = 0;

    virtual ~UE_FLD() { }
};

/** @brief Creates a smart pointer to a FastLineDetector object and initializes it

@param _length_threshold    10         - Segment shorter than this will be discarded
@param _distance_threshold  1.41421356 - A point placed from a hypothesis line
                                            segment farther than this will be
                                            regarded as an outlier
@param _canny_th1           50         - First threshold for
                                            hysteresis procedure in Canny()
@param _canny_th2           50         - Second threshold for
                                            hysteresis procedure in Canny()
@param _canny_aperture_size 3          - Aperturesize for the sobel
                                            operator in Canny()
@param _do_merge            false      - If true, incremental merging of segments
                                            will be perfomred
*/
Ptr<UE_FLD> createUE_FLD(
	int _length_threshold = 10, float _distance_threshold = 1.414213562f,
	double _canny_th1 = 50.0, double _canny_th2 = 50.0, int _canny_aperture_size = 3,
	bool _do_merge = false);
        
//! @} ximgproc_fast_line_detector
     

struct SEGMENT
{
    float x1, y1, x2, y2, angle;
};
class UE_FLDImpl : public UE_FLD
{
public:
    /**
        * @param _length_threshold    10         - Segment shorter than this will be discarded
        * @param _distance_threshold  1.41421356 - A point placed from a hypothesis line segment
        *                                          farther than this will be regarded as an outlier
        * @param _canny_th1           50         - First threshold for
        *        _                                 hysteresis procedure in Canny()
        * @param _canny_th2           50         - Second threshold for
        *        _                                 hysteresis procedure in Canny()
        * @param _canny_aperture_size 3          - Aperturesize for the sobel
        *        _                                 operator in Canny()
        * @param _do_merge            false      - If true, incremental merging of segments
                                                will be perfomred
        */
    UE_FLDImpl(int _length_threshold = 10, float _distance_threshold = 1.414213562f,
        double _canny_th1 = 50.0, double _canny_th2 = 50.0, int _canny_aperture_size = 3,
        bool _do_merge = false);

    /**
        * Detect lines in the input image.
        *
        * @param _image    A grayscale(CV_8UC1) input image.
        *                  If only a roi needs to be selected, use
        *                  lsd_ptr->detect(image(roi), ..., lines);
        *                  lines += Scalar(roi.x, roi.y, roi.x, roi.y);
        * @param _lines    Return: A vector of Vec4f elements specifying the beginning and ending point of
        *                  a line. Where Vec4f is (x1, y1, x2, y2), point 1 is the start, point 2 is the end.
        *                  Returned lines are directed so that the brighter side is placed on left.
        */
    void detect(InputArray _image, TArray<Vec4f>& _lines) CV_OVERRIDE;

    /**
        * Draw lines on the given canvas.
        *
        * @param image          The image, where lines will be drawn
        *                       Should have the size of the image, where the lines were found
        * @param lines          The lines that need to be drawn
        * @param draw_arrow     If true, arrow heads will be drawn
        */
    void drawSegments(InputOutputArray _image, InputArray lines, bool draw_arrow = false) CV_OVERRIDE;

private:
    int imagewidth, imageheight, threshold_length;
    float threshold_dist;
    double canny_th1, canny_th2;
    int canny_aperture_size;
    bool do_merge;

    UE_FLDImpl& operator= (const UE_FLDImpl&); // to quiet MSVC
    template<class T>
    void incidentPoint(const Mat& l, T& pt);

    void mergeLines(const SEGMENT& seg1, const SEGMENT& seg2, SEGMENT& seg_merged);

    bool mergeSegments(const SEGMENT& seg1, const SEGMENT& seg2, SEGMENT& seg_merged);

    bool getPointChain(const Mat& img, Point pt, Point& chained_pt, float& direction, int step);

    double distPointLine(const Mat& p, Mat& l);

    void extractSegments(const std::vector<Point2i>& points, std::vector<SEGMENT>& segments);

    void lineDetection(const Mat& src, std::vector<SEGMENT>& segments_all);

    void pointInboardTest(const Mat& src, Point2i& pt);

    inline void getAngle(SEGMENT& seg);

    void additionalOperationsOnSegment(const Mat& src, SEGMENT& seg);

    void drawSegment(Mat& mat, const SEGMENT& seg, Scalar bgr = Scalar(0, 255, 0),
        int thickness = 1, bool directed = true);
};