// readfacevmd - reads facial expression from photo / movie and generate a VMD motion file.

// OpenFace Headers
#include <LandmarkCoreIncludes.h>
#include <FaceAnalyser.h>
#include <SequenceCapture.h>
#include <GazeEstimation.h>

#include <opencv2/core/core.hpp>
#include <boost/filesystem.hpp>
#include <dlib/image_processing/frontal_face_detector.h>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "smooth_reduce.h"
#include "MMDFileIOUtil.h"
#include "VMD.h"
#include "morph_name.h"
#include "refine.h"

#define _USE_MATH_DEFINES
#include <math.h>

#define RFV_DLL_EXPORT
#include "readfacevmd.h"

using namespace std;
using namespace Eigen;

const int LANDMARK_NUM = 68;

// Action Unit ID
enum AUID {
  InnerBrowRaiser = 1,      // AU01 眉の内側を上げる
  OuterBrowRaiser = 2,      // AU02 眉の外側を上げる
  BrowLowerer = 4,          // AU04 眉を下げる
  UpperLidRaiser = 5,       // AU05 目を見開く
  CheekRaiser = 6,          // AU06 頬を上げる
  LidTightener = 7,         // AU07 細目
  NoseWrinkler = 9,         // AU09 鼻に皴を寄せる。怒り
  UpperLipRaiser = 10,      // AU10 上唇を上げる
  LipCornerPuller = 12,     // AU12 口の端を上げる。にやり
  Dimpler = 14,             // AU14 えくぼ
  LipCornerDepressor = 15,  // AU15 への字口
  ChinRaiser = 17,          // AU17 顎を上げる
  LipStrecher = 20,         // AU20 口を横に伸ばす
  LipTightener = 23,        // AU23 口をすぼめる
  LipPart = 25,             // AU25 口を開ける。「い」の口でもtrueになる
  JawDrop = 26,             // AU26 顎を下げる。「あ」の口の判定にはこちらを使う
  LipSuck = 28,             // AU28 唇を吸う
  Blink = 45,               // AU45 まばたき
};
const int AU_SIZE = 46;
const double ACTION_UNIT_MAXVAL = 5.0;

void dumprot(const Quaterniond& rot, string name)
{
  Vector3d v = rot.toRotationMatrix().eulerAngles(0, 1, 2);
  cout << name << ": " << v.x() * 180 / M_PI << "," << v.y()  * 180 / M_PI << "," << v.z()  * 180 / M_PI << endl;
}


// 回転のキーフレームを VMD_Frame の vector に追加する
void add_rotation_pose(vector<VMD_Frame>& frame_vec, const Quaterniond& rot, uint32_t frame_number, string bone_name)
{
    VMD_Frame frame;
    MMDFileIOUtil::utf8_to_sjis(bone_name, frame.bonename, frame.bonename_len);
    frame.number = frame_number;
    frame.rotation.w() = rot.w();
    frame.rotation.x() = rot.x();
    frame.rotation.y() = rot.y();
    frame.rotation.z() = rot.z();
    frame_vec.push_back(frame);
}

// 移動のキーフレームを VMD_Frame の vector に追加する
void add_position_pose(vector<VMD_Frame>& frame_vec, const Vector3f& pos, uint32_t frame_number, string bone_name)
{
    VMD_Frame frame;
    MMDFileIOUtil::utf8_to_sjis(bone_name, frame.bonename, frame.bonename_len);
    frame.number = frame_number;
    frame.position.x() = pos.x();
    frame.position.y() = pos.y();
    frame.position.z() = pos.z();
    frame_vec.push_back(frame);
}

// 頭の向き(回転)のキーフレームを VMD_Frame の vector に格納する
void add_head_pose(vector<VMD_Frame>& frame_vec, const Quaterniond& rot, uint32_t frame_number)
{
  string bone_name = u8"頭";
  add_rotation_pose(frame_vec, rot, frame_number, bone_name);
}

// センターの位置のキーフレームを VMD_Frame の vector に格納する
void add_center_frame(vector<VMD_Frame>& frame_vec, const Vector3f& pos, uint32_t frame_number)
{
  string bone_name = u8"センター";
  add_position_pose(frame_vec, pos, frame_number, bone_name);
}

// 目の向き(回転)のキーフレームを VMD_Frame の vector に追加する
void add_gaze_pose(vector<VMD_Frame>& frame_vec, cv::Point3f gazedir_left, cv::Point3f gazedir_right,
		   const Quaterniond& head_rot, uint32_t frame_number)
{
  Vector3d front = head_rot * Vector3d(0, 0, -1);
  Vector3d leftdir;
  leftdir.x() = gazedir_left.x;
  leftdir.y() = - gazedir_left.y;
  leftdir.z() = gazedir_left.z;
  Quaterniond rot_left = Quaterniond::FromTwoVectors(front, leftdir);
  Vector3d rightdir;
  rightdir.x() = gazedir_right.x;
  rightdir.y() = - gazedir_right.y;
  rightdir.z() = gazedir_right.z;
  Quaterniond rot_right = Quaterniond::FromTwoVectors(front, rightdir);

  // 目の回転量を補正
  // TODO: 補正係数の適切な値を決める
  const double amp_both = 1.0;
  const double amp_each = 0.25;
  rot_right = Quaterniond::Identity().slerp(amp_each, rot_right);
  rot_left = Quaterniond::Identity().slerp(amp_each, rot_left);

  add_rotation_pose(frame_vec, rot_right, frame_number, u8"左目");
  add_rotation_pose(frame_vec, rot_left, frame_number, u8"右目");
}

// 表情フレームを VMD_Morph の vector に追加する 
void add_morph_frame(vector<VMD_Morph>& morph_vec, string name, uint32_t frame_number, float weight)
{
  if (weight > 1.0) {
    weight = 1.0;
  }
  if (weight < 0.0) {
    weight = 0.0;
  }
  VMD_Morph morph;
  MMDFileIOUtil::utf8_to_sjis(name, morph.name, morph.name_len);
  morph.frame = frame_number;
  morph.weight = weight;
  morph_vec.push_back(morph);
}

// 顔の動きを表すAction Unitをface_analyserから取り出す
void get_action_unit(double* au, FaceAnalysis::FaceAnalyser face_analyser) {
  for (int i = 0; i < AU_SIZE; i++) {
    au[i] = 0;
  }

  auto intensity = face_analyser.GetCurrentAUsReg();
  auto presence = face_analyser.GetCurrentAUsClass();

  bool valid[AU_SIZE];

  for (auto& p : presence) {
    string id_str = p.first.substr(2, 2); // 数字部分を取り出す
    int id = atoi(id_str.c_str());
    if (p.second == 0) {
      valid[id] = false;
    } else {
      valid[id] = true;
    }
  }
  
  for (auto& p : intensity) {
    string id_str = p.first.substr(2, 2); // 数字部分を取り出す
    int id = atoi(id_str.c_str());
    if (valid[id]) {
      au[id] = p.second / ACTION_UNIT_MAXVAL;
      // cout << "AU" << id << ": " << au[id] << endl;
    }
  }
}

// 顔の表情を推定して morph_vec に追加する
void estimate_facial_expression(vector<VMD_Morph>& morph_vec, double* au, uint32_t frame_number)
{
  // 口
  double mouth_a = au[AUID::JawDrop] * 2;
  double mouth_i = 0;
  double mouth_u = au[AUID::LipTightener] * 2;
  if (mouth_a < 0.1 && mouth_u < 0.1) {
    mouth_i = au[AUID::LipPart] * 2;
  }
  double mouth_smile = au[AUID::LipCornerPuller];

  add_morph_frame(morph_vec, u8"あ", frame_number, mouth_a);
  add_morph_frame(morph_vec, u8"い", frame_number, mouth_i);
  add_morph_frame(morph_vec, u8"う", frame_number, mouth_u);
  add_morph_frame(morph_vec, u8"にやり", frame_number, mouth_smile);
  add_morph_frame(morph_vec, u8"∧", frame_number, au[AUID::LipCornerDepressor]);

  // 目
  double blink = au[AUID::LidTightener];
  if (au[AUID::Blink] > 0.2) {
    blink = 1.0;
  }
  add_morph_frame(morph_vec, u8"まばたき", frame_number, blink);
  // まばたき/笑いの切り替えは後処理で行う
  add_morph_frame(morph_vec, u8"CheekRaiser", frame_number, au[AUID::CheekRaiser]);

  add_morph_frame(morph_vec, u8"びっくり", frame_number, au[AUID::UpperLidRaiser]);

  // 眉
  add_morph_frame(morph_vec, u8"困る", frame_number, au[AUID::InnerBrowRaiser]);
  // 困る/にこりの切り替えは後処理で行う
  add_morph_frame(morph_vec, u8"真面目", frame_number, au[AUID::OuterBrowRaiser]);
  add_morph_frame(morph_vec, u8"怒り", frame_number, au[AUID::NoseWrinkler]);
  add_morph_frame(morph_vec, u8"下", frame_number, au[AUID::BrowLowerer]);
  add_morph_frame(morph_vec, u8"上", frame_number, au[AUID::UpperLidRaiser]);
}

void init_vmd_header(VMD_Header& h)
{
  memset(h.version, 0, h.version_len);
  strcpy(h.version, "Vocaloid Motion Data 0002");
  memset(h.modelname, 0, h.modelname_len);
  strcpy(h.modelname, "dummy model");
}


// Custom gaze esitmater
// following functions are based on: https://github.com/TadasBaltrusaitis/OpenFace/
cv::Matx33f Euler2RotationMatrix(const cv::Vec3f& eulerAngles)
{
    cv::Matx33f rotation_matrix;

    float s1 = sin(eulerAngles[0]);
    float s2 = sin(eulerAngles[1]);
    float s3 = sin(eulerAngles[2]);

    float c1 = cos(eulerAngles[0]);
    float c2 = cos(eulerAngles[1]);
    float c3 = cos(eulerAngles[2]);

    rotation_matrix(0, 0) = c2 * c3;
    rotation_matrix(0, 1) = -c2 * s3;
    rotation_matrix(0, 2) = s2;
    rotation_matrix(1, 0) = c1 * s3 + c3 * s1 * s2;
    rotation_matrix(1, 1) = c1 * c3 - s1 * s2 * s3;
    rotation_matrix(1, 2) = -c2 * s1;
    rotation_matrix(2, 0) = s1 * s3 - c1 * c3 * s2;
    rotation_matrix(2, 1) = c3 * s1 + c1 * s2 * s3;
    rotation_matrix(2, 2) = c1 * c2;

    return rotation_matrix;
}

cv::Point3f GetPupilPosition(cv::Mat_<float> eyeLdmks3d)
{
    eyeLdmks3d = eyeLdmks3d.t();
    cv::Mat_<float> irisLdmks3d = eyeLdmks3d.rowRange(0, 8);
    cv::Point3f p(mean(irisLdmks3d.col(0))[0], mean(irisLdmks3d.col(1))[0], mean(irisLdmks3d.col(2))[0]);
    return p;
}

void CustomEstimateGaze(const LandmarkDetector::CLNF& clnf_model, cv::Point3f& gaze_absolute, float fx, float fy, float cx, float cy, bool left_eye)
{
    cv::Vec6f headPose = LandmarkDetector::GetPose(clnf_model, fx, fy, cx, cy);
    cv::Vec3f eulerAngles(headPose(3), headPose(4), headPose(5));
    cv::Matx33f rotMat = Euler2RotationMatrix(eulerAngles);

    int part = -1;
    for (size_t i = 0; i < clnf_model.hierarchical_models.size(); ++i)
    {
        if (left_eye && clnf_model.hierarchical_model_names[i].compare("left_eye_28") == 0)
        {
            part = i;
        }
        if (!left_eye && clnf_model.hierarchical_model_names[i].compare("right_eye_28") == 0)
        {
            part = i;
        }
    }

    if (part == -1)
    {
        std::cout << "Couldn't find the eye model, something wrong" << std::endl;
        gaze_absolute = cv::Point3f(0, 0, 0);
        return;
    }

    cv::Mat eyeLdmks3d = clnf_model.hierarchical_models[part].GetShape(fx, fy, cx, cy);

    cv::Point3f pupil = GetPupilPosition(eyeLdmks3d);
    cv::Point3f rayDir = pupil / norm(pupil);

    cv::Mat faceLdmks3d = clnf_model.GetShape(fx, fy, cx, cy);
    faceLdmks3d = faceLdmks3d.t();

    int eyeIdx = 1;
    if (left_eye)
    {
        eyeIdx = 0;
    }

    cv::Mat offsetMat = (cv::Mat_<float>(3, 1) << 0, -3.5, 7.0);
    cv::Mat offsetMatT = (cv::Mat(rotMat) * offsetMat).t();
    cv::Point3f eyeOffset = cv::Point3f(offsetMatT);

    cv::Mat mat36 = faceLdmks3d.row(36 + eyeIdx * 6);
    cv::Mat mat39 = faceLdmks3d.row(39 + eyeIdx * 6);
    cv::Point3f eyelidL = cv::Point3f(mat36);
    cv::Point3f eyelidR = cv::Point3f(mat39);
    cv::Point3f eyeCentre = (eyelidL + eyelidR) / 2.0f;
    cv::Point3f eyeballCentre = eyeCentre + eyeOffset;

    // 2Dに再投影
    float d = eyeCentre.z;
    float l2dx = eyelidL.x * d / eyelidL.z;
    float l2dy = eyelidL.y * d / eyelidL.z;
    float r2dx = eyelidR.x * d / eyelidR.z;
    float r2dy = eyelidR.y * d / eyelidR.z;
    float p2dx = pupil.x * d / pupil.z;
    float p2dy = pupil.y * d / pupil.z;
    float t = (p2dx - r2dx) / (l2dx - r2dx);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    float newZ = eyelidR.z + (eyelidL.z - eyelidR.z) * t;
    // 新しいzで、黒目の中心位置を再計算する。
    pupil.x = pupil.x * newZ / pupil.z;
    pupil.y = pupil.y * newZ / pupil.z;
    pupil.z = newZ;
    rayDir = pupil / norm(pupil);

    //cv::Point3f gazeVecAxis = RaySphereIntersect(cv::Point3f(0, 0, 0), rayDir, eyeballCentre, 12) - eyeballCentre;
    cv::Point3f gazeVecAxis = pupil - eyeballCentre;

    gaze_absolute = gazeVecAxis / norm(gazeVecAxis);
}
// Custom gaze esitmater

// image_file_name で指定された画像/動画ファイルから表情を推定して vmd_file_name に出力する
RFV_DLL_DECL int read_face_vmd(const std::string& image_file_name, const std::string& vmd_file_name,
			       float cutoff_freq, float threshold_pos, float threshold_rot, float threshold_morph,
			       const std::string& nameconf_file_name)
{
  map<string, string> rename_map;
  if (nameconf_file_name.length() != 0) {
    rename_map = make_rename_map(string(nameconf_file_name));
  }
  
  vector<string> arg_str;
  arg_str.push_back("-f");
  arg_str.push_back(image_file_name);
  Utilities::SequenceCapture cap;
  if (! cap.Open(arg_str)) {
    cerr << "Open error" << endl;
    return 1;
  }

  LandmarkDetector::FaceModelParameters model_parameters;
  LandmarkDetector::CLNF face_model(model_parameters.model_location);

  // for Action Unit
  FaceAnalysis::FaceAnalyserParameters face_analysis_params;
  face_analysis_params.OptimizeForImages();
  FaceAnalysis::FaceAnalyser face_analyser(face_analysis_params);

  VMD vmd;
  init_vmd_header(vmd.header);

  float srcfps = cap.fps;
  float tgtfps = 30.0;

  for (uint32_t frame_number = 0; true; frame_number++) {
    cout << "frame:" << frame_number << endl;
    cv::Mat image = cap.GetNextFrame();
    if (image.empty()) {
      break;
    }
    cv::Mat_<uchar> grayscale_image = cap.GetGrayFrame();

    if (! LandmarkDetector::DetectLandmarksInVideo(image, face_model, model_parameters, grayscale_image)) {
      continue;
    }

    // 頭の向きを推定する
    cv::Vec6d head_pose = LandmarkDetector::GetPose(face_model, cap.fx, cap.fy, cap.cx, cap.cy);
    Quaterniond rot_vmd = AngleAxisd(- head_pose[3], Vector3d::UnitX())
      * AngleAxisd(head_pose[4], Vector3d::UnitY())
      * AngleAxisd(- head_pose[5], Vector3d::UnitZ());
    add_head_pose(vmd.frame, rot_vmd, frame_number);
    Vector3f center_pos(head_pose[0], - head_pose[1], (head_pose[2] - 1000));
    center_pos = center_pos * 12.5 / 1000 / 2; // 1m = 12.5ミクセル
    add_center_frame(vmd.frame, center_pos, frame_number);

    // 表情を推定する
    face_analyser.PredictStaticAUsAndComputeFeatures(image, face_model.detected_landmarks);
    double action_unit[AU_SIZE];
    get_action_unit(action_unit, face_analyser);
    estimate_facial_expression(vmd.morph, action_unit, frame_number);

    // 目の向きを推定する
    if (face_model.eye_model) {
      cv::Point3f gazedir_left(0, 0, -1);
      cv::Point3f gazedir_right(0, 0, -1);
      CustomEstimateGaze(face_model, gazedir_left, cap.fx, cap.fy, cap.cx, cap.cy, true);
      CustomEstimateGaze(face_model, gazedir_right, cap.fx, cap.fy, cap.cx, cap.cy, false);

      add_gaze_pose(vmd.frame, gazedir_left, gazedir_right, rot_vmd, frame_number);
    }
  }

  cout << "smoothing & reduction start" << endl;
  cout << "cutoff frequency: " << cutoff_freq << endl;
  cout << "position threshold: " << threshold_pos << endl;
  cout << "rotation threshold: " << threshold_rot << endl;
  cout << "morph threshold: " << threshold_morph << endl;
  smooth_and_reduce(vmd, cutoff_freq, threshold_pos, threshold_rot, threshold_morph,
                    srcfps, tgtfps, false);
  cout << "smoothing & reduction end" << endl;

  refine_morph(vmd);

  cout << "rename morph & bone" << endl;
  rename_morph(vmd, rename_map);
  rename_frame(vmd, rename_map);
  
  cout << "VMD output start" << endl;
  cout << "output filename: " << vmd_file_name << endl;
  // VMDファイルを書き出す
  ofstream out(vmd_file_name, ios::binary);
  vmd.output(out);
  out.close();
  cout << "VMD output end" << endl;
  return 0;
}

