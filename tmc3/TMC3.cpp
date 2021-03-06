/* The copyright in this software is being made available under the BSD
 * Licence, included below.  This software may be subject to other third
 * party and contributor rights, including patent rights, and no such
 * rights are granted under this licence.
 *
 * Copyright (c) 2017-2018, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the ISO/IEC nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "TMC3.h"

#include <memory>

#include "PCCTMC3Encoder.h"
#include "PCCTMC3Decoder.h"
#include "constants.h"
#include "ply.h"
#include "pointset_processing.h"
#include "program_options_lite.h"
#include "io_tlv.h"
#include "version.h"

using namespace std;
using namespace pcc;

//============================================================================

struct Parameters {
  bool isDecoder;

  // command line parsing should adjust dist2 values according to PQS
  bool positionQuantizationScaleAdjustsDist2;

  // output mode for ply writing (binary or ascii)
  bool outputBinaryPly;

  // output ply resolution in points per metre (or 0 for undefined)
  float outputResolution;

  // when true, configure the encoder as if no attributes are specified
  bool disableAttributeCoding;

  // Frame number of first file in input sequence.
  int firstFrameNum;

  // Number of frames to process.
  int frameCount;

  std::string uncompressedDataPath;
  std::string compressedStreamPath;
  std::string reconstructedDataPath;

  // Filename for saving recoloured point cloud (encoder).
  std::string postRecolorPath;

  // Filename for saving pre inverse scaled point cloud (decoder).
  std::string preInvScalePath;

  pcc::EncoderParams encoder;
  pcc::DecoderParams decoder;

  // perform attribute colourspace conversion on ply input/output.
  bool convertColourspace;

  // todo(df): this should be per-attribute
  int reflectanceScale;

  // resort the input points by azimuth angle
  bool sortInputByAzimuth;
};

//----------------------------------------------------------------------------

class SequenceEncoder : public PCCTMC3Encoder3::Callbacks {
public:
  // NB: params must outlive the lifetime of the decoder.
  SequenceEncoder(Parameters* params);

  int compress(Stopwatch* clock);

  // determine the output ply scale factor
  double outputScale(const SequenceParameterSet& sps);

protected:
  int compressOneFrame(Stopwatch* clock);

  void onOutputBuffer(const PayloadBuffer& buf) override;
  void onPostRecolour(const PCCPointSet3& cloud) override;

private:
  ply::PropertyNameMap _plyAttrNames;

  // The raw origin used for input sorting
  Vec3<int> _angularOrigin;

  Parameters* params;
  PCCTMC3Encoder3 encoder;

  std::ofstream bytestreamFile;

  int frameNum;
};

//----------------------------------------------------------------------------

class SequenceDecoder : public PCCTMC3Decoder3::Callbacks {
public:
  // NB: params must outlive the lifetime of the decoder.
  SequenceDecoder(const Parameters* params);

  int decompress(Stopwatch* clock);

  // determine the output ply scale factor
  double outputScale(const SequenceParameterSet& sps);

protected:
  void onOutputCloud(
    const SequenceParameterSet& sps,
    const PCCPointSet3& decodedPointCloud) override;

private:
  const Parameters* params;
  PCCTMC3Decoder3 decoder;

  std::ofstream bytestreamFile;

  int frameNum;
  Stopwatch* clock;
};

//============================================================================

void convertToGbr(const SequenceParameterSet& sps, PCCPointSet3& cloud);
void convertFromGbr(const SequenceParameterSet& sps, PCCPointSet3& cloud);

//============================================================================

int
main(int argc, char* argv[])
{
  cout << "MPEG PCC tmc3 version " << ::pcc::version << endl;

  Parameters params;

  try {
    if (!ParseParameters(argc, argv, params))
      return 1;
  }
  catch (df::program_options_lite::ParseFailure& e) {
    std::cerr << "Error parsing option \"" << e.arg << "\" with argument \""
              << e.val << "\"." << std::endl;
    return 1;
  }

  // Timers to count elapsed wall/user time
  pcc::chrono::Stopwatch<std::chrono::steady_clock> clock_wall;
  pcc::chrono::Stopwatch<pcc::chrono::utime_inc_children_clock> clock_user;

  clock_wall.start();

  int ret = 0;
  if (params.isDecoder) {
    ret = SequenceDecoder(&params).decompress(&clock_user);
  } else {
    ret = SequenceEncoder(&params).compress(&clock_user);
  }

  clock_wall.stop();

  using namespace std::chrono;
  auto total_wall = duration_cast<milliseconds>(clock_wall.count()).count();
  auto total_user = duration_cast<milliseconds>(clock_user.count()).count();
  std::cout << "Processing time (wall): " << total_wall / 1000.0 << " s\n";
  std::cout << "Processing time (user): " << total_user / 1000.0 << " s\n";

  return ret;
}

//---------------------------------------------------------------------------

std::array<const char*, 3>
axisOrderToPropertyNames(AxisOrder order)
{
  static const std::array<const char*, 3> kAxisOrderToPropertyNames[] = {
    {"z", "y", "x"}, {"x", "y", "z"}, {"x", "z", "y"}, {"y", "z", "x"},
    {"z", "y", "x"}, {"z", "x", "y"}, {"y", "x", "z"}, {"x", "y", "z"},
  };

  return kAxisOrderToPropertyNames[int(order)];
}

//---------------------------------------------------------------------------
// :: Command line / config parsing helpers

template<typename T>
static std::istream&
readUInt(std::istream& in, T& val)
{
  unsigned int tmp;
  in >> tmp;
  val = T(tmp);
  return in;
}

namespace pcc {
static std::istream&
operator>>(std::istream& in, ColourMatrix& val)
{
  return readUInt(in, val);
}
}  // namespace pcc

namespace pcc {
static std::istream&
operator>>(std::istream& in, AxisOrder& val)
{
  return readUInt(in, val);
}
}  // namespace pcc

namespace pcc {
static std::istream&
operator>>(std::istream& in, AttributeEncoding& val)
{
  return readUInt(in, val);
}
}  // namespace pcc

namespace pcc {
static std::istream&
operator>>(std::istream& in, PartitionMethod& val)
{
  return readUInt(in, val);
}
}  // namespace pcc

namespace pcc {
static std::istream&
operator>>(std::istream& in, PredGeomEncOpts::SortMode& val)
{
  return readUInt(in, val);
}
}  // namespace pcc

namespace pcc {
static std::ostream&
operator<<(std::ostream& out, const ColourMatrix& val)
{
  switch (val) {
  case ColourMatrix::kIdentity: out << "0 (Identity)"; break;
  case ColourMatrix::kBt709: out << "1 (Bt709)"; break;
  case ColourMatrix::kUnspecified: out << "2 (Unspecified)"; break;
  case ColourMatrix::kReserved_3: out << "3 (Reserved)"; break;
  case ColourMatrix::kUsa47Cfr73dot682a20:
    out << "4 (Usa47Cfr73dot682a20)";
    break;
  case ColourMatrix::kBt601: out << "5 (Bt601)"; break;
  case ColourMatrix::kSmpte170M: out << "6 (Smpte170M)"; break;
  case ColourMatrix::kSmpte240M: out << "7 (Smpte240M)"; break;
  case ColourMatrix::kYCgCo: out << "8 (kYCgCo)"; break;
  case ColourMatrix::kBt2020Ncl: out << "9 (Bt2020Ncl)"; break;
  case ColourMatrix::kBt2020Cl: out << "10 (Bt2020Cl)"; break;
  case ColourMatrix::kSmpte2085: out << "11 (Smpte2085)"; break;
  default: out << "Unknown"; break;
  }
  return out;
}
}  // namespace pcc

namespace pcc {
static std::ostream&
operator<<(std::ostream& out, const AxisOrder& val)
{
  switch (val) {
  case AxisOrder::kZYX: out << "0 (zyx)"; break;
  case AxisOrder::kXYZ: out << "1 (xyz)"; break;
  case AxisOrder::kXZY: out << "2 (xzy)"; break;
  case AxisOrder::kYZX: out << "3 (yzx)"; break;
  case AxisOrder::kZYX_4: out << "4 (zyx)"; break;
  case AxisOrder::kZXY: out << "5 (zxy)"; break;
  case AxisOrder::kYXZ: out << "6 (yxz)"; break;
  case AxisOrder::kXYZ_7: out << "7 (xyz)"; break;
  }
  return out;
}
}  // namespace pcc

namespace pcc {
static std::ostream&
operator<<(std::ostream& out, const AttributeEncoding& val)
{
  switch (val) {
  case AttributeEncoding::kPredictingTransform: out << "0 (Pred)"; break;
  case AttributeEncoding::kRAHTransform: out << "1 (RAHT)"; break;
  case AttributeEncoding::kLiftingTransform: out << "2 (Lift)"; break;
  }
  return out;
}
}  // namespace pcc

namespace pcc {
static std::ostream&
operator<<(std::ostream& out, const PartitionMethod& val)
{
  switch (val) {
  case PartitionMethod::kNone: out << "0 (None)"; break;
  case PartitionMethod::kUniformGeom: out << "2 (UniformGeom)"; break;
  case PartitionMethod::kOctreeUniform: out << "3 (UniformOctree)"; break;
  case PartitionMethod::kUniformSquare: out << "4 (UniformSquare)"; break;
  default: out << int(val) << " (Unknown)"; break;
  }
  return out;
}
}  // namespace pcc

namespace pcc {
static std::ostream&
operator<<(std::ostream& out, const PredGeomEncOpts::SortMode& val)
{
  switch (val) {
    using SortMode = PredGeomEncOpts::SortMode;
  case SortMode::kNoSort: out << int(val) << " (None)"; break;
  case SortMode::kSortMorton: out << int(val) << " (Morton)"; break;
  case SortMode::kSortAzimuth: out << int(val) << " (Azimuth)"; break;
  case SortMode::kSortRadius: out << int(val) << " (Radius)"; break;
  default: out << int(val) << " (Unknown)"; break;
  }
  return out;
}
}  // namespace pcc

namespace df {
namespace program_options_lite {
  template<typename T>
  struct option_detail<pcc::Vec3<T>> {
    static constexpr bool is_container = true;
    static constexpr bool is_fixed_size = true;
    typedef T* output_iterator;

    static void clear(pcc::Vec3<T>& container){};
    static output_iterator make_output_iterator(pcc::Vec3<T>& container)
    {
      return &container[0];
    }
  };
}  // namespace program_options_lite
}  // namespace df

//---------------------------------------------------------------------------
// :: Command line / config parsing

bool
ParseParameters(int argc, char* argv[], Parameters& params)
{
  namespace po = df::program_options_lite;

  struct {
    AttributeDescription desc;
    AttributeParameterSet aps;
    EncoderAttributeParams encoder;
  } params_attr;

  bool print_help = false;

  // a helper to set the attribute
  std::function<po::OptionFunc::Func> attribute_setter =
    [&](po::Options&, const std::string& name, po::ErrorReporter) {
      // copy the current state of parsed attribute parameters
      //
      // NB: this does not cause the default values of attr to be restored
      // for the next attribute block.  A side-effect of this is that the
      // following is allowed leading to attribute foo having both X=1 and
      // Y=2:
      //   "--attr.X=1 --attribute foo --attr.Y=2 --attribute foo"
      //

      // NB: insert returns any existing element
      const auto& it = params.encoder.attributeIdxMap.insert(
        {name, int(params.encoder.attributeIdxMap.size())});

      if (it.second) {
        params.encoder.sps.attributeSets.push_back(params_attr.desc);
        params.encoder.aps.push_back(params_attr.aps);
        params.encoder.attr.push_back(params_attr.encoder);
        return;
      }

      // update existing entry
      params.encoder.sps.attributeSets[it.first->second] = params_attr.desc;
      params.encoder.aps[it.first->second] = params_attr.aps;
      params.encoder.attr[it.first->second] = params_attr.encoder;
    };

  /* clang-format off */
  // The definition of the program/config options, along with default values.
  //
  // NB: when updating the following tables:
  //      (a) please keep to 80-columns for easier reading at a glance,
  //      (b) do not vertically align values -- it breaks quickly
  //
  po::Options opts;
  opts.addOptions()
  ("help", print_help, false, "this help text")
  ("config,c", po::parseConfigFile, "configuration file name")

  (po::Section("General"))

  ("mode", params.isDecoder, false,
    "The encoding/decoding mode:\n"
    "  0: encode\n"
    "  1: decode")

  // i/o parameters
  ("firstFrameNum",
     params.firstFrameNum, 0,
     "Frame number for use with interpolating %d format specifiers"
     "in input/output filenames")

  ("frameCount",
     params.frameCount, 1,
     "Number of frames to encode")

  ("reconstructedDataPath",
    params.reconstructedDataPath, {},
    "The ouput reconstructed pointcloud file path (decoder only)")

  ("uncompressedDataPath",
    params.uncompressedDataPath, {},
    "The input pointcloud file path")

  ("compressedStreamPath",
    params.compressedStreamPath, {},
    "The compressed bitstream path (encoder=output, decoder=input)")

  ("postRecolorPath",
    params.postRecolorPath, {},
    "Recolored pointcloud file path (encoder only)")

  ("preInvScalePath",
    params.preInvScalePath, {},
    "Pre inverse scaled pointcloud file path (decoder only)")

  ("outputBinaryPly",
    params.outputBinaryPly, false,
    "Output ply files using binary (or otherwise ascii) format")

  ("outputResolution",
    params.outputResolution, -1.f,
    "Resolution of output point cloud in points per metre")

  ("convertPlyColourspace",
    params.convertColourspace, true,
    "Convert ply colourspace according to attribute colourMatrix")

  // general
  // todo(df): this should be per-attribute
  ("hack.reflectanceScale",
    params.reflectanceScale, 1,
    "scale factor to be applied to reflectance "
    "pre encoding / post reconstruction")

  (po::Section("Decoder"))

  ("skipOctreeLayers",
    params.decoder.minGeomNodeSizeLog2, 0,
    " 0   : Full decode. \n"
    " N>0 : Skip the bottom N layers in decoding process.\n"
    " skipLayerNum indicates the number of skipped lod layers from leaf lod.")

  (po::Section("Encoder"))

  ("sortInputByAzimuth",
    params.sortInputByAzimuth, false,
    "Sort input points by azimuth angle")

  ("geometry_axis_order",
    params.encoder.sps.geometry_axis_order, AxisOrder::kXYZ,
    "Sets the geometry axis coding order:\n"
    "  0: (zyx)\n  1: (xyz)\n  2: (xzy)\n"
    "  3: (yzx)\n  4: (zyx)\n  5: (zxy)\n"
    "  6: (yxz)\n  7: (xyz)")

  // NB: the underlying variable is in STV order.
  //     Conversion happens during argument sanitization.
  ("seq_bounding_box_xyz0",
    params.encoder.sps.seqBoundingBoxOrigin, {0},
    "Origin (x,y,z) of the sequence bounding box. "
    "NB: seq_bounding_box_whd must be set for paramter to have an effect")

  // NB: the underlying variable is in STV order.
  //     Conversion happens during argument sanitization.
  ("seq_bounding_box_whd",
    params.encoder.sps.seqBoundingBoxSize, {0},
    "seq_bounding_box_whd")

  ("srcResolution",
    params.encoder.srcResolution, 0.f,
    "Resolution of source point cloud in points per metre")

  ("positionQuantizationScale",
    params.encoder.geomPreScale, 1.f,
    "Scale factor to be applied to point positions during pre-processing")

  ("positionQuantizationScaleAdjustsDist2",
    params.positionQuantizationScaleAdjustsDist2, false,
    "Scale dist2 values by squared positionQuantizationScale")

  ("mergeDuplicatedPoints",
    params.encoder.gps.geom_unique_points_flag, true,
    "Enables removal of duplicated points")

  ("partitionMethod",
    params.encoder.partition.method, PartitionMethod::kUniformSquare,
    "Method used to partition input point cloud into slices/tiles:\n"
    "  0: none\n"
    "  1: none (deprecated)\n"
    "  2: n Uniform-Geometry partition bins along the longest edge\n"
    "  3: Uniform Geometry partition at n octree depth\n"
    "  4: Uniform Square partition")

  ("partitionOctreeDepth",
    params.encoder.partition.octreeDepth, 1,
    "Depth of octree partition for partitionMethod=4")

  ("sliceMaxPoints",
    params.encoder.partition.sliceMaxPoints, 1100000,
    "Maximum number of points per slice")

  ("sliceMinPoints",
    params.encoder.partition.sliceMinPoints, 550000,
    "Minimum number of points per slice (soft limit)")

  ("tileSize",
    params.encoder.partition.tileSize, 0,
    "Partition input into cubic tiles of given size")

  ("cabac_bypass_stream_enabled_flag",
    params.encoder.sps.cabac_bypass_stream_enabled_flag, false,
    "Controls coding method for ep(bypass) bins")

  ("disableAttributeCoding",
    params.disableAttributeCoding, false,
    "Ignore attribute coding configuration")

  ("enforceLevelLimits",
    params.encoder.enforceLevelLimits, true,
    "Abort if level limits exceeded")

  (po::Section("Geometry"))

  ("geomTreeType",
    params.encoder.gps.predgeom_enabled_flag, false,
    "Selects the tree coding method\n"
    "  0: octree\n"
    "  1: predictive")

  ("qtbtEnabled",
    params.encoder.gps.qtbt_enabled_flag, true,
    "Enables non-cubic geometry bounding box")

  ("maxNumQtBtBeforeOt",
    params.encoder.geom.qtbt.maxNumQtBtBeforeOt, 4,
    "Max number of qtbt partitions before ot")

  ("minQtbtSizeLog2",
    params.encoder.geom.qtbt.minQtbtSizeLog2, 0,
    "Minimum size of qtbt partitions")

  ("numOctreeEntropyStreams",
    // NB: this is adjusted by minus 1 after the arguments are parsed
    params.encoder.gbh.geom_stream_cnt_minus1, 1,
    "Number of entropy streams for octree coding")

  ("bitwiseOccupancyCoding",
    params.encoder.gps.bitwise_occupancy_coding_flag, true,
    "Selects between bitwise and bytewise occupancy coding:\n"
    "  0: bytewise\n"
    "  1: bitwise")

  ("neighbourContextRestriction",
    params.encoder.gps.neighbour_context_restriction_flag, false,
    "Limit geometry octree occupancy contextualisation to sibling nodes")

  ("neighbourAvailBoundaryLog2",
    params.encoder.gps.neighbour_avail_boundary_log2, 0,
    "Defines the avaliability volume for neighbour occupancy lookups."
    " 0: unconstrained")

  ("inferredDirectCodingMode",
    params.encoder.gps.inferred_direct_coding_mode_enabled_flag, true,
    "Permits early termination of the geometry octree for isolated points")

  ("adjacentChildContextualization",
    params.encoder.gps.adjacent_child_contextualization_enabled_flag, true,
    "Occupancy contextualization using neighbouring adjacent children")

  ("intra_pred_max_node_size_log2",
    params.encoder.gps.intra_pred_max_node_size_log2, 0,
    "octree nodesizes eligible for occupancy intra prediction")

  ("planarEnabled",
    params.encoder.gps.geom_planar_mode_enabled_flag, true,
    "Use planar mode for geometry coding")

  ("planarModeThreshold0",
    params.encoder.gps.geom_planar_threshold0, 77,
    "Activation threshold (0-127) of first planar mode. "
    "Lower values imply more use of the first planar mode")

  ("planarModeThreshold1",
    params.encoder.gps.geom_planar_threshold1, 99,
    "Activation threshold (0-127) of second planar mode. "
    "Lower values imply more use of the first planar mode")

  ("planarModeThreshold2",
    params.encoder.gps.geom_planar_threshold2, 113,
    "Activation threshold (0-127) of third planar mode. "
    "Lower values imply more use of the third planar mode")

   ("planarModeIdcmUse",
    params.encoder.gps.geom_planar_idcm_threshold, 0,
    "Degree (0-127) of IDCM activation when planar mode is enabled.\n"
    "  0 => never, 127 => always")

  ("trisoup_node_size_log2",
    params.encoder.gps.trisoup_node_size_log2, 0,
    "Size of nodes for surface triangulation.\n"
    "  0: disabled\n")

  ("trisoup_sampling_value",
    params.encoder.gps.trisoup_sampling_value, 0,
    "Trisoup voxelisation sampling rate\n"
    "  0: automatic")

  ("positionQuantisationEnabled",
    params.encoder.gps.geom_scaling_enabled_flag, false,
    "Enable in-loop quantisation of positions")

  ("positionBaseQp",
    params.encoder.gps.geom_base_qp, 0,
    "Base QP used in position quantisation (0 = lossless)")

  ("positionIdcmQp",
    params.encoder.idcmQp, 0,
    "QP used in position quantisation of IDCM nodes")

  ("positionSliceQpOffset",
    params.encoder.gbh.geom_slice_qp_offset, 0,
    "Per-slice QP offset used in position quantisation")

  ("positionQuantisationOctreeDepth",
    params.encoder.gbh.geom_octree_qp_offset_depth, -1,
    "Octree depth used for signalling position QP offsets (-1 => disabled)")

  ("angularEnabled",
    params.encoder.gps.geom_angular_mode_enabled_flag, false,
    "Controls angular contextualisation of occupancy")

  // NB: the underlying variable is in STV order.
  //     Conversion happens during argument sanitization.
  ("lidarHeadPosition",
    params.encoder.gps.geomAngularOrigin, {0, 0, 0},
    "laser head position (x, y, z) in angular mode")

  ("numLasers",
    params.encoder.numLasers, 0,
    "Number of lasers in angular mode")

  ("lasersTheta",
    params.encoder.lasersTheta, {},
    "Vertical laser angle in angular mode")

  ("lasersZ",
    params.encoder.lasersZ, {},
    "Vertical laser offset in angular mode")

  ("lasersNumPhiPerTurn",
    params.encoder.gps.geom_angular_num_phi_per_turn, {},
    "Number of sampling poisitions in a complete laser turn in angular mode")

  ("planarBufferDisabled",
    params.encoder.gps.planar_buffer_disabled_flag, false,
    "Disable planar buffer (when angular mode is enabled)")

  ("predGeomSort",
    params.encoder.predGeom.sortMode, PredGeomEncOpts::kSortMorton,
    "Predictive geometry tree construction order")

  ("predGeomTreePtsMax",
    params.encoder.predGeom.maxPtsPerTree, 1100000,
    "Maximum number of points per predictive geometry tree")

  (po::Section("Attributes"))

  // attribute processing
  //   NB: Attribute options are special in the way they are applied (see above)
  ("attribute",
    attribute_setter,
    "Encode the given attribute (NB, must appear after the"
    "following attribute parameters)")

  ("bitdepth",
    params_attr.desc.bitdepth, 8,
    "Attribute bitdepth")

  ("defaultValue",
    params_attr.desc.attr_default_value, {},
    "Default attribute component value(s) in case of data omission")

  // todo(df): this should be per-attribute
  ("colourMatrix",
    params_attr.desc.cicp_matrix_coefficients_idx, ColourMatrix::kBt709,
    "Matrix used in colourspace conversion\n"
    "  0: none (identity)\n"
    "  1: ITU-T BT.709\n"
    "  8: YCgCo")

  ("transformType",
    params_attr.aps.attr_encoding, AttributeEncoding::kPredictingTransform,
    "Coding method to use for attribute:\n"
    "  0: Hierarchical neighbourhood prediction\n"
    "  1: Region Adaptive Hierarchical Transform (RAHT)\n"
    "  2: Hierarichical neighbourhood prediction as lifting transform")

  ("rahtPredictionEnabled",
    params_attr.aps.raht_prediction_enabled_flag, true,
    "Controls the use of transform-domain prediction")

  ("rahtPredictionThreshold0",
    params_attr.aps.raht_prediction_threshold0, 2,
    "Grandparent threshold for early transform-domain prediction termination")

  ("rahtPredictionThreshold1",
    params_attr.aps.raht_prediction_threshold1, 6,
    "Parent threshold for early transform-domain prediction termination")

  // NB: the cli option sets +1, the minus1 will be applied later
  ("numberOfNearestNeighborsInPrediction",
    params_attr.aps.num_pred_nearest_neighbours_minus1, 3,
    "Attribute's maximum number of nearest neighbors to be used for prediction")

  ("adaptivePredictionThreshold",
    params_attr.aps.adaptive_prediction_threshold, -1,
    "Neighbouring attribute value difference that enables choice of "
    "single|multi predictors. Applies to transformType=2 only.\n"
    "  -1: auto = 2**(bitdepth-2)")

  ("attributeSearchRange",
    params_attr.aps.search_range, 128,
    "Range for nearest neighbor search")

  // NB: the underlying variable is in STV order.
  //     Conversion happens during argument sanitization.
  ("lod_neigh_bias",
    params_attr.aps.lodNeighBias, {1, 1, 1},
    "Attribute's (x, y, z) component intra prediction weights")

  ("lodDecimation",
    params_attr.aps.lod_decimation_enabled_flag, false,
    "Controls LoD generation method:\n"
    " 0: distance based subsampling\n"
    " 1: periodic subsampling using lodSamplingPeriod")

  ("max_num_direct_predictors",
    params_attr.aps.max_num_direct_predictors, 3,
    "Maximum number of nearest neighbour candidates used in direct"
    "attribute prediction")

  ("levelOfDetailCount",
    params_attr.aps.num_detail_levels, 1,
    "Attribute's number of levels of detail")

  ("dist2",
    params_attr.aps.dist2, {},
    "List of per LoD squared distances used in LoD generation.\n"
    " 0 entries: derive base value automatically\n"
    ">0 entries: derive subsequent values automatically")

  ("lodSamplingPeriod",
    params_attr.aps.lodSamplingPeriod, {4},
    "List of per LoD sampling periods used in LoD generation.\n")

  ("intraLodPredictionEnabled",
    params_attr.aps.intra_lod_prediction_enabled_flag, false,
    "Permits referring to points in same LoD")

  ("interComponentPredictionEnabled",
    params_attr.aps.inter_component_prediction_enabled_flag, false,
    "Use primary attribute component to predict values of subsequent "
    "components")

  ("canonical_point_order_flag",
    params_attr.aps.canonical_point_order_flag, false,
    "Enable skipping morton sort in case of number of LoD equal to 1")

  ("aps_scalable_enable_flag",
    params_attr.aps.scalable_lifting_enabled_flag, false,
    "Enable scalable attritube coding")

  ("max_neigh_range",
    params_attr.aps.max_neigh_range, 5,
    "maximum nearest neighbour range for scalable lifting")

  ("qp",
    // NB: this is adjusted with minus 4 after the arguments are parsed
    params_attr.aps.init_qp_minus4, 4,
    "Attribute's luma quantisation parameter")

  ("qpChromaOffset",
    params_attr.aps.aps_chroma_qp_offset, 0,
    "Attribute's chroma quantisation parameter offset (relative to luma)")

  ("aps_slice_qp_deltas_present_flag",
    params_attr.aps.aps_slice_qp_deltas_present_flag, false,
    "Enable signalling of per-slice QP values")

  ("qpLayerOffsetsLuma",
    params_attr.encoder.abh.attr_layer_qp_delta_luma, {},
      "Attribute's per layer luma QP offsets")

  ("qpLayerOffsetsChroma",
      params_attr.encoder.abh.attr_layer_qp_delta_chroma, {},
      "Attribute's per layer chroma QP offsets")

  // This section is just dedicated to attribute recolouring (encoder only).
  // parameters are common to all attributes.
  (po::Section("Recolouring"))

  ("recolourSearchRange",
    params.encoder.recolour.searchRange, 1,
    "")

  ("recolourNumNeighboursFwd",
    params.encoder.recolour.numNeighboursFwd, 8,
    "")

  ("recolourNumNeighboursBwd",
    params.encoder.recolour.numNeighboursBwd, 1,
    "")

  ("recolourUseDistWeightedAvgFwd",
    params.encoder.recolour.useDistWeightedAvgFwd, true,
    "")

  ("recolourUseDistWeightedAvgBwd",
    params.encoder.recolour.useDistWeightedAvgBwd, true,
    "")

  ("recolourSkipAvgIfIdenticalSourcePointPresentFwd",
    params.encoder.recolour.skipAvgIfIdenticalSourcePointPresentFwd, true,
    "")

  ("recolourSkipAvgIfIdenticalSourcePointPresentBwd",
    params.encoder.recolour.skipAvgIfIdenticalSourcePointPresentBwd, false,
    "")

  ("recolourDistOffsetFwd",
    params.encoder.recolour.distOffsetFwd, 4.,
    "")

  ("recolourDistOffsetBwd",
    params.encoder.recolour.distOffsetBwd, 4.,
    "")

  ("recolourMaxGeometryDist2Fwd",
    params.encoder.recolour.maxGeometryDist2Fwd, 1000.,
    "")

  ("recolourMaxGeometryDist2Bwd",
    params.encoder.recolour.maxGeometryDist2Bwd, 1000.,
    "")

  ("recolourMaxAttributeDist2Fwd",
    params.encoder.recolour.maxAttributeDist2Fwd, 1000.,
    "")

  ("recolourMaxAttributeDist2Bwd",
    params.encoder.recolour.maxAttributeDist2Bwd, 1000.,
    "")

  ;
  /* clang-format on */

  po::setDefaults(opts);
  po::ErrorReporter err;
  const list<const char*>& argv_unhandled =
    po::scanArgv(opts, argc, (const char**)argv, err);

  for (const auto arg : argv_unhandled) {
    err.warn() << "Unhandled argument ignored: " << arg << "\n";
  }

  if (argc == 1 || print_help) {
    po::doHelp(std::cout, opts, 78);
    return false;
  }

  // fix the representation of various options
  params.encoder.gbh.geom_stream_cnt_minus1--;
  for (auto& attr_aps : params.encoder.aps) {
    attr_aps.init_qp_minus4 -= 4;
    attr_aps.num_pred_nearest_neighbours_minus1--;
  }

  // set default output resolution (this works for the decoder too)
  if (params.outputResolution < 0)
    params.outputResolution = params.encoder.srcResolution;

  // convert coordinate systems if the coding order is different from xyz
  convertXyzToStv(&params.encoder.sps);
  convertXyzToStv(params.encoder.sps, &params.encoder.gps);
  for (auto& aps : params.encoder.aps)
    convertXyzToStv(params.encoder.sps, &aps);

  // Certain coding modes are not available when trisoup is enabled.
  // Disable them, and warn if set (they may be set as defaults).
  if (params.encoder.gps.trisoup_node_size_log2 > 0) {
    if (!params.encoder.gps.geom_unique_points_flag)
      err.warn() << "TriSoup geometry does not preserve duplicated points\n";

    if (params.encoder.gps.inferred_direct_coding_mode_enabled_flag)
      err.warn() << "TriSoup geometry is incompatable with IDCM\n";

    params.encoder.gps.geom_unique_points_flag = true;
    params.encoder.gps.inferred_direct_coding_mode_enabled_flag = false;
  }

  // tweak qtbt generation when trisoup is /isn't enabled
  params.encoder.geom.qtbt.trisoupNodeSizeLog2 =
    params.encoder.gps.trisoup_node_size_log2;

  // Planar coding mode is not available for bytewise coding
  if (!params.encoder.gps.bitwise_occupancy_coding_flag) {
    if (params.encoder.gps.geom_planar_mode_enabled_flag)
      err.warn() << "Bytewise geometry coding does not support planar mode\n";
    params.encoder.gps.geom_planar_mode_enabled_flag = false;
  }

  // support disabling attribute coding (simplifies configuration)
  if (params.disableAttributeCoding) {
    params.encoder.attributeIdxMap.clear();
    params.encoder.sps.attributeSets.clear();
    params.encoder.aps.clear();
  }

  // fixup any per-attribute settings
  for (const auto& it : params.encoder.attributeIdxMap) {
    auto& attr_sps = params.encoder.sps.attributeSets[it.second];
    auto& attr_aps = params.encoder.aps[it.second];
    auto& attr_enc = params.encoder.attr[it.second];

    // default values for attribute
    attr_sps.attr_instance_id = 0;
    attr_sps.cicp_colour_primaries_idx = 2;
    attr_sps.cicp_transfer_characteristics_idx = 2;
    attr_sps.cicp_video_full_range_flag = true;
    attr_sps.cicpParametersPresent = false;
    attr_sps.source_attr_offset_log2 = 0;
    attr_sps.source_attr_scale_log2 = 0;
    attr_sps.scalingParametersPresent = false;

    if (it.first == "reflectance") {
      // Avoid wasting bits signalling chroma quant step size for reflectance
      attr_aps.aps_chroma_qp_offset = 0;
      attr_enc.abh.attr_layer_qp_delta_chroma.clear();

      // There is no matrix for reflectace
      attr_sps.cicp_matrix_coefficients_idx = ColourMatrix::kUnspecified;
      attr_sps.attr_num_dimensions_minus1 = 0;
      attr_sps.attributeLabel = KnownAttributeLabel::kReflectance;
    }

    if (it.first == "color") {
      attr_sps.attr_num_dimensions_minus1 = 2;
      attr_sps.attributeLabel = KnownAttributeLabel::kColour;
      attr_sps.cicpParametersPresent = true;
    }

    // Derive the secondary bitdepth
    // todo(df): this needs to be a command line argument
    //  -- but there are a few edge cases to handle
    attr_sps.bitdepthSecondary = attr_sps.bitdepth;

    // Assume that YCgCo is actually YCgCoR for now
    if (attr_sps.cicp_matrix_coefficients_idx == ColourMatrix::kYCgCo)
      attr_sps.bitdepthSecondary++;

    // Extend the default attribute value to the correct width if present
    if (!attr_sps.attr_default_value.empty())
      attr_sps.attr_default_value.resize(
        attr_sps.attr_num_dimensions_minus1 + 1,
        attr_sps.attr_default_value.back());

    // derive the dist2 values based on an initial value
    if (attr_aps.lodParametersPresent()) {
      if (attr_aps.dist2.size() > attr_aps.num_detail_levels) {
        attr_aps.dist2.resize(attr_aps.num_detail_levels);
      } else if (
        attr_aps.dist2.size() < attr_aps.num_detail_levels
        && !attr_aps.dist2.empty()) {
        if (attr_aps.dist2.size() < attr_aps.num_detail_levels) {
          attr_aps.dist2.resize(attr_aps.num_detail_levels);
          const double distRatio = 4.0;
          uint64_t d2 = attr_aps.dist2[0];
          for (int i = 0; i < attr_aps.num_detail_levels; ++i) {
            attr_aps.dist2[i] = d2;
            d2 = uint64_t(std::round(distRatio * d2));
          }
        }
      }
    }
    // In order to simplify specification of dist2 values, which are
    // depending on the scale of the coded point cloud, the following
    // adjust the dist2 values according to PQS.  The user need only
    // specify the unquantised PQS value.
    if (params.positionQuantizationScaleAdjustsDist2) {
      double pqs = params.encoder.geomPreScale;
      double pqs2 = pqs * pqs;
      for (auto& dist2 : attr_aps.dist2)
        dist2 = int64_t(std::round(pqs2 * dist2));
    }

    // derive samplingPeriod values based on initial value
    if (
      !attr_aps.lodParametersPresent()
      || !attr_aps.lod_decimation_enabled_flag) {
      attr_aps.lodSamplingPeriod.clear();
    } else if (!attr_aps.lodSamplingPeriod.empty()) {
      auto i = attr_aps.lodSamplingPeriod.size();
      attr_aps.lodSamplingPeriod.resize(attr_aps.num_detail_levels);
      // add any extra values as required
      for (; i < attr_aps.num_detail_levels; i++)
        attr_aps.lodSamplingPeriod[i] = attr_aps.lodSamplingPeriod[i - 1];
    }

    // Set default threshold based on bitdepth
    if (attr_aps.adaptive_prediction_threshold == -1) {
      attr_aps.adaptive_prediction_threshold = 1 << (attr_sps.bitdepth - 2);
    }

    if (attr_aps.attr_encoding == AttributeEncoding::kLiftingTransform) {
      attr_aps.adaptive_prediction_threshold = 0;
      attr_aps.intra_lod_prediction_enabled_flag = false;
    }

    // For RAHT, ensure that the unused lod count = 0 (prevents mishaps)
    if (attr_aps.attr_encoding == AttributeEncoding::kRAHTransform) {
      attr_aps.num_detail_levels = 0;
      attr_aps.adaptive_prediction_threshold = 0;
    }
  }

  // convert floating point values of Lasers' Theta and H to fixed point
  if (params.encoder.gps.geom_angular_mode_enabled_flag) {
    for (auto val : params.encoder.lasersTheta) {
      int one = 1 << 18;
      params.encoder.gps.geom_angular_theta_laser.push_back(round(val * one));
    }

    for (auto val : params.encoder.lasersZ) {
      int one = 1 << 3;
      params.encoder.gps.geom_angular_z_laser.push_back(
        round(val * params.encoder.geomPreScale * one));
    }

    if (
      params.encoder.gps.geom_angular_theta_laser.size()
      != params.encoder.numLasers)
      err.error() << "lasersZ.size() != numLasers\n";

    if (
      params.encoder.gps.geom_angular_z_laser.size()
      != params.encoder.numLasers)
      err.error() << "lasersTheta.size() != numLasers\n";

    if (
      params.encoder.gps.geom_angular_num_phi_per_turn.size()
      != params.encoder.numLasers)
      err.error() << "lasersNumPhiPerTurn.size() != numLasers\n";

    if (params.encoder.gps.qtbt_enabled_flag) {
      params.encoder.geom.qtbt.angularMaxNodeMinDimLog2ToSplitV =
        std::max<int>(0, 8 + log2(params.encoder.geomPreScale));
      params.encoder.geom.qtbt.angularMaxDiffToSplitZ =
        std::max<int>(0, 1 + log2(params.encoder.geomPreScale));
    }
  }

  // tweak qtbt when angular is / isn't enabled
  params.encoder.geom.qtbt.angularTweakEnabled =
    params.encoder.gps.geom_angular_mode_enabled_flag;

  // sanity checks

  if (
    params.encoder.gps.planar_buffer_disabled_flag
    && !params.encoder.gps.geom_angular_mode_enabled_flag)
    err.error() << "planar buffer can only be disabled with angular mode\n";

  if (
    params.encoder.partition.sliceMaxPoints
    < params.encoder.partition.sliceMinPoints)
    err.error()
      << "sliceMaxPoints must be greater than or equal to sliceMinPoints\n";

  if (params.encoder.gps.intra_pred_max_node_size_log2)
    if (!params.encoder.gps.neighbour_avail_boundary_log2)
      err.error() << "Geometry intra prediction requires finite"
                     "neighbour_avail_boundary_log2\n";

  for (const auto& it : params.encoder.attributeIdxMap) {
    const auto& attr_sps = params.encoder.sps.attributeSets[it.second];
    const auto& attr_aps = params.encoder.aps[it.second];
    auto& attr_enc = params.encoder.attr[it.second];

    if (it.first == "color") {
      if (
        attr_enc.abh.attr_layer_qp_delta_luma.size()
        != attr_enc.abh.attr_layer_qp_delta_chroma.size()) {
        err.error() << it.first
                    << ".qpLayerOffsetsLuma length != .qpLayerOffsetsChroma\n";
      }
    }

    if (attr_sps.bitdepth > 16)
      err.error() << it.first << ".bitdepth must be less than 17\n";

    if (attr_sps.bitdepthSecondary > 16)
      err.error() << it.first << ".bitdepth_secondary must be less than 17\n";

    if (attr_aps.lodParametersPresent()) {
      int lod = attr_aps.num_detail_levels;
      if (lod > 255 || lod < 0) {
        err.error() << it.first
                    << ".levelOfDetailCount must be in the range [0,255]\n";
      }

      // if empty, values are derived automatically
      if (!attr_aps.dist2.empty() && attr_aps.dist2.size() != lod) {
        err.error() << it.first << ".dist2 does not have " << lod
                    << " entries\n";
      }

      if (lod > 0 && attr_aps.canonical_point_order_flag) {
        err.error() << it.first
                    << "when levelOfDetailCount > 0, "
                       "canonicalPointOrder must be 0\n";
      }

      if (
        attr_aps.lod_decimation_enabled_flag
        && attr_aps.lodSamplingPeriod.empty()) {
        err.error() << it.first
                    << ".lodSamplingPeriod must contain at least one entry\n";
      }

      for (auto samplingPeriod : attr_aps.lodSamplingPeriod) {
        if (samplingPeriod < 2)
          err.error() << it.first << ".lodSamplingPeriod values must be > 1\n";
      }

      if (attr_aps.adaptive_prediction_threshold < 0) {
        err.error() << it.first
                    << ".adaptivePredictionThreshold must be positive\n";
      }

      if (
        attr_aps.num_pred_nearest_neighbours_minus1
        >= kAttributePredictionMaxNeighbourCount) {
        err.error() << it.first
                    << ".numberOfNearestNeighborsInPrediction must be <= "
                    << kAttributePredictionMaxNeighbourCount << "\n";
      }
      if (attr_aps.scalable_lifting_enabled_flag) {
        if (attr_aps.attr_encoding != AttributeEncoding::kLiftingTransform) {
          err.error() << it.first << "AttributeEncoding must be "
                      << (int)AttributeEncoding::kLiftingTransform << "\n";
        }

        if (attr_aps.lod_decimation_enabled_flag) {
          err.error() << it.first
                      << ".lod_decimation_enabled_flag must be = 0 \n";
        }

        if (params.encoder.gps.trisoup_node_size_log2 > 0) {
          err.error() << it.first
                      << "trisoup_node_size_log2 must be disabled \n";
        }
      }
    }

    if (attr_aps.init_qp_minus4 < 0 || attr_aps.init_qp_minus4 + 4 > 51)
      err.error() << it.first << ".qp must be in the range [4,51]\n";

    if (std::abs(attr_aps.aps_chroma_qp_offset) > 51 - 4) {
      err.error() << it.first
                  << ".qpChromaOffset must be in the range [-47,47]\n";
    }
  }

  // check required arguments are specified

  if (!params.isDecoder && params.uncompressedDataPath.empty())
    err.error() << "uncompressedDataPath not set\n";

  if (params.isDecoder && params.reconstructedDataPath.empty())
    err.error() << "reconstructedDataPath not set\n";

  if (params.compressedStreamPath.empty())
    err.error() << "compressedStreamPath not set\n";

  // report the current configuration (only in the absence of errors so
  // that errors/warnings are more obvious and in the same place).
  if (err.is_errored)
    return false;

  // Dump the complete derived configuration
  cout << "+ Effective configuration parameters\n";

  po::dumpCfg(cout, opts, "General", 4);
  if (params.isDecoder) {
    po::dumpCfg(cout, opts, "Decoder", 4);
  } else {
    po::dumpCfg(cout, opts, "Encoder", 4);
    po::dumpCfg(cout, opts, "Geometry", 4);
    po::dumpCfg(cout, opts, "Recolouring", 4);

    for (const auto& it : params.encoder.attributeIdxMap) {
      // NB: when dumping the config, opts references params_attr
      params_attr.desc = params.encoder.sps.attributeSets[it.second];
      params_attr.aps = params.encoder.aps[it.second];
      params_attr.encoder = params.encoder.attr[it.second];
      cout << "    " << it.first << "\n";
      po::dumpCfg(cout, opts, "Attributes", 8);
    }
  }

  cout << endl;

  return true;
}

//============================================================================

SequenceEncoder::SequenceEncoder(Parameters* params) : params(params)
{
  // determine the naming (ordering) of ply properties
  _plyAttrNames.position =
    axisOrderToPropertyNames(params->encoder.sps.geometry_axis_order);

  // NB: this is the raw origin before the encoder tweaks it
  _angularOrigin = params->encoder.gps.geomAngularOrigin;
}

//----------------------------------------------------------------------------

double
SequenceEncoder::outputScale(const SequenceParameterSet& sps)
{
  switch (sps.seq_geom_scale_unit_flag) {
  case ScaleUnit::kPointsPerMetre:
    // todo(df): warn if output resolution not specified?
    return params->outputResolution > 0
      ? params->outputResolution / sps.seq_geom_scale
      : 1.;

  case ScaleUnit::kDimensionless: return 1. / sps.seq_geom_scale;
  }
}

//----------------------------------------------------------------------------

int
SequenceEncoder::compress(Stopwatch* clock)
{
  bytestreamFile.open(params->compressedStreamPath, ios::binary);
  if (!bytestreamFile.is_open()) {
    return -1;
  }

  const int lastFrameNum = params->firstFrameNum + params->frameCount;
  for (frameNum = params->firstFrameNum; frameNum < lastFrameNum; frameNum++) {
    if (compressOneFrame(clock))
      return -1;
  }

  std::cout << "Total bitstream size " << bytestreamFile.tellp() << " B\n";
  bytestreamFile.close();

  return 0;
}

//----------------------------------------------------------------------------

int
SequenceEncoder::compressOneFrame(Stopwatch* clock)
{
  std::string srcName{expandNum(params->uncompressedDataPath, frameNum)};
  PCCPointSet3 pointCloud;
  if (
    !ply::read(srcName, _plyAttrNames, pointCloud)
    || pointCloud.getPointCount() == 0) {
    cout << "Error: can't open input file!" << endl;
    return -1;
  }

  // Some evaluations wish to scan the points in azimuth order to simulate
  // real-time acquisition (since the input has lost its original order).
  if (params->sortInputByAzimuth)
    sortByAzimuth(pointCloud, 0, pointCloud.getPointCount(), _angularOrigin);

  // Sanitise the input point cloud
  // todo(df): remove the following with generic handling of properties
  bool codeColour = params->encoder.attributeIdxMap.count("color");
  if (!codeColour)
    pointCloud.removeColors();
  assert(codeColour == pointCloud.hasColors());

  bool codeReflectance = params->encoder.attributeIdxMap.count("reflectance");
  if (!codeReflectance)
    pointCloud.removeReflectances();
  assert(codeReflectance == pointCloud.hasReflectances());

  clock->start();

  if (params->convertColourspace)
    convertFromGbr(params->encoder.sps, pointCloud);

  if (params->reflectanceScale > 1 && pointCloud.hasReflectances()) {
    const auto pointCount = pointCloud.getPointCount();
    for (size_t i = 0; i < pointCount; ++i) {
      int val = pointCloud.getReflectance(i) / params->reflectanceScale;
      pointCloud.setReflectance(i, val);
    }
  }

  // The reconstructed point cloud
  std::unique_ptr<PCCPointSet3> reconPointCloud;
  if (!params->reconstructedDataPath.empty()) {
    reconPointCloud.reset(new PCCPointSet3);
  }

  auto bytestreamLenFrameStart = bytestreamFile.tellp();

  int ret = encoder.compress(
    pointCloud, &params->encoder, this, reconPointCloud.get());
  if (ret) {
    cout << "Error: can't compress point cloud!" << endl;
    return -1;
  }

  auto bytestreamLenFrameEnd = bytestreamFile.tellp();
  int frameLen = bytestreamLenFrameEnd - bytestreamLenFrameStart;

  std::cout << "Total frame size " << frameLen << " B" << std::endl;

  clock->stop();

  if (!params->reconstructedDataPath.empty()) {
    if (params->convertColourspace)
      convertToGbr(params->encoder.sps, *reconPointCloud);

    if (params->reflectanceScale > 1 && reconPointCloud->hasReflectances()) {
      const auto pointCount = reconPointCloud->getPointCount();
      for (size_t i = 0; i < pointCount; ++i) {
        int val =
          reconPointCloud->getReflectance(i) * params->reflectanceScale;
        reconPointCloud->setReflectance(i, val);
      }
    }

    std::string recName{expandNum(params->reconstructedDataPath, frameNum)};
    auto plyScale = outputScale(params->encoder.sps);
    auto plyOrigin = params->encoder.sps.seqBoundingBoxOrigin * plyScale;
    ply::write(
      *reconPointCloud, _plyAttrNames, plyScale, plyOrigin, recName,
      !params->outputBinaryPly);
  }

  return 0;
}

//----------------------------------------------------------------------------

void
SequenceEncoder::onOutputBuffer(const PayloadBuffer& buf)
{
  writeTlv(buf, bytestreamFile);
}

//----------------------------------------------------------------------------

void
SequenceEncoder::onPostRecolour(const PCCPointSet3& cloud)
{
  if (params->postRecolorPath.empty()) {
    return;
  }

  std::string plyName{expandNum(params->postRecolorPath, frameNum)};
  auto plyScale = outputScale(params->encoder.sps);
  auto plyOrigin = params->encoder.sps.seqBoundingBoxOrigin * plyScale;

  // todo(df): stop the clock
  if (!params->convertColourspace) {
    ply::write(
      cloud, _plyAttrNames, plyScale, plyOrigin, plyName,
      !params->outputBinaryPly);
    return;
  }

  PCCPointSet3 tmpCloud(cloud);
  convertToGbr(params->encoder.sps, tmpCloud);
  ply::write(
    tmpCloud, _plyAttrNames, plyScale, plyOrigin, plyName,
    !params->outputBinaryPly);
}

//============================================================================

SequenceDecoder::SequenceDecoder(const Parameters* params)
  : params(params), decoder(params->decoder)
{}

//----------------------------------------------------------------------------

double
SequenceDecoder::outputScale(const SequenceParameterSet& sps)
{
  switch (sps.seq_geom_scale_unit_flag) {
  case ScaleUnit::kPointsPerMetre:
    // todo(df): warn if output resolution not specified?
    return params->outputResolution > 0
      ? params->outputResolution / sps.seq_geom_scale
      : 1.;

  case ScaleUnit::kDimensionless: return 1. / sps.seq_geom_scale;
  }
}

//----------------------------------------------------------------------------

int
SequenceDecoder::decompress(Stopwatch* clock)
{
  ifstream fin(params->compressedStreamPath, ios::binary);
  if (!fin.is_open()) {
    return -1;
  }

  frameNum = params->firstFrameNum;
  this->clock = clock;

  PayloadBuffer buf;

  clock->start();

  while (true) {
    PayloadBuffer* buf_ptr = &buf;
    readTlv(fin, &buf);

    // at end of file (or other error), flush decoder
    if (!fin)
      buf_ptr = nullptr;

    if (decoder.decompress(buf_ptr, this)) {
      cout << "Error: can't decompress point cloud!" << endl;
      return -1;
    }

    if (!buf_ptr)
      break;
  }

  fin.clear();
  fin.seekg(0, ios_base::end);
  std::cout << "Total bitstream size " << fin.tellg() << " B" << std::endl;

  clock->stop();

  return 0;
}

//----------------------------------------------------------------------------

void
SequenceDecoder::onOutputCloud(
  const SequenceParameterSet& sps, const PCCPointSet3& decodedPointCloud)
{
  // copy the point cloud in order to modify it according to the output options
  PCCPointSet3 pointCloud(decodedPointCloud);

  if (params->convertColourspace)
    convertToGbr(sps, pointCloud);

  if (params->reflectanceScale > 1 && pointCloud.hasReflectances()) {
    const auto pointCount = pointCloud.getPointCount();
    for (size_t i = 0; i < pointCount; ++i) {
      int val = pointCloud.getReflectance(i) * params->reflectanceScale;
      pointCloud.setReflectance(i, val);
    }
  }

  // the order of the property names must be determined from the sps
  ply::PropertyNameMap attrNames;
  attrNames.position = axisOrderToPropertyNames(sps.geometry_axis_order);

  // Dump the decoded colour using the pre inverse scaled geometry
  if (!params->preInvScalePath.empty()) {
    std::string filename{expandNum(params->preInvScalePath, frameNum)};
    ply::write(
      pointCloud, attrNames, 1.0, 0.0, params->preInvScalePath,
      !params->outputBinaryPly);
  }

  clock->stop();

  auto plyScale = outputScale(sps);
  auto plyOrigin = sps.seqBoundingBoxOrigin * plyScale;
  std::string decName{expandNum(params->reconstructedDataPath, frameNum)};
  if (!ply::write(
        pointCloud, attrNames, plyScale, plyOrigin, decName,
        !params->outputBinaryPly)) {
    cout << "Error: can't open output file!" << endl;
  }

  clock->start();

  // todo(df): frame number should be derived from the bitstream
  frameNum++;
}

//============================================================================

const AttributeDescription*
findColourAttrDesc(const SequenceParameterSet& sps)
{
  // todo(df): don't assume that there is only one colour attribute in the sps
  for (const auto& desc : sps.attributeSets) {
    if (desc.attributeLabel == KnownAttributeLabel::kColour)
      return &desc;
  }
  return nullptr;
}

//----------------------------------------------------------------------------

void
convertToGbr(const SequenceParameterSet& sps, PCCPointSet3& cloud)
{
  const AttributeDescription* attrDesc = findColourAttrDesc(sps);
  if (!attrDesc)
    return;

  switch (attrDesc->cicp_matrix_coefficients_idx) {
  case ColourMatrix::kBt709: convertYCbCrBt709ToGbr(cloud); break;

  case ColourMatrix::kYCgCo:
    // todo(df): select YCgCoR vs YCgCo
    convertYCgCoRToGbr(attrDesc->bitdepth, cloud);
    break;

  default: break;
  }
}

//----------------------------------------------------------------------------

void
convertFromGbr(const SequenceParameterSet& sps, PCCPointSet3& cloud)
{
  const AttributeDescription* attrDesc = findColourAttrDesc(sps);
  if (!attrDesc)
    return;

  switch (attrDesc->cicp_matrix_coefficients_idx) {
  case ColourMatrix::kBt709: convertGbrToYCbCrBt709(cloud); break;

  case ColourMatrix::kYCgCo:
    // todo(df): select YCgCoR vs YCgCo
    convertGbrToYCgCoR(attrDesc->bitdepth, cloud);
    break;

  default: break;
  }
}

//============================================================================
