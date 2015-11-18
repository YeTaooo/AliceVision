/* 
 * File:   main_voctreeLocalizer.cpp
 * Author: sgaspari
 *
 * Created on September 12, 2015, 3:16 PM
 */
#include <openMVG/localization/VoctreeLocalizer.hpp>
#include <openMVG/localization/LocalizationResult.hpp>
#include <openMVG/localization/optimization.hpp>
#include <openMVG/sfm/pipelines/localization/SfM_Localizer.hpp>
#include <openMVG/image/image_io.hpp>
#include <openMVG/dataio/FeedProvider.hpp>
#include <openMVG/features/image_describer.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/progress.hpp>
#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/sum.hpp>

#include <iostream>
#include <string>
#include <chrono>

#if HAVE_ALEMBIC
#include <openMVG/dataio/AlembicExporter.hpp>
#endif // HAVE_ALEMBIC

#define POPART_COUT(x) std::cout << x << std::endl
#define POPART_CERR(x) std::cerr << x << std::endl


namespace bfs = boost::filesystem;
namespace bacc = boost::accumulators;
namespace po = boost::program_options;

using namespace openMVG;


std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  std::stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

int main(int argc, char** argv)
{
  std::string calibFile;            //< the calibration file
  std::string sfmFilePath;          //< the OpenMVG .json data file
  std::string descriptorsFolder;    //< the OpenMVG .json data file
  std::string vocTreeFilepath;      //< the vocabulary tree file
  std::string weightsFilepath;      //< the vocabulary tree weights file
  std::string mediaFilepath;        //< the media file to localize
  localization::VoctreeLocalizer::Parameters param = localization::VoctreeLocalizer::Parameters();
  std::string preset = features::describerPreset_enumToString(param._featurePreset);               //< the preset for the feature extractor
#if HAVE_ALEMBIC
  std::string exportFile = "trackedcameras.abc"; //!< the export file
#endif
#if HAVE_CCTAG
  bool useSIFT_CCTAG = false;
#endif
  std::string algostring = "FirstBest";
  bool globalBundle = false;      ///< If !param._refineIntrinsics it can run a final global budndle to refine the scene

  po::options_description desc(
                               "This program takes as input a media (image, image sequence, video) and a database (voctree, 3D structure data) \n"
                               "and returns for each frame a pose estimation for the camera.");
  desc.add_options()
      ("help,h", "Print this message")
      ("results,r", po::value<size_t>(&param._numResults)->default_value(param._numResults), "Number of images to retrieve in database")
      ("commonviews,", po::value<size_t>(&param._numCommonViews)->default_value(param._numCommonViews), "Number of minimum images in which a point must be seen to be used in cluster tracking")
      ("preset,", po::value<std::string>(&preset)->default_value(preset), "Number of minimum images in which a point must be seen to be used in cluster tracking")
      ("calibration,c", po::value<std::string>(&calibFile)/*->required( )*/, "Calibration file")
      ("voctree,t", po::value<std::string>(&vocTreeFilepath)->required(), "Filename for the vocabulary tree")
      ("weights,w", po::value<std::string>(&weightsFilepath), "Filename for the vocabulary tree weights")
      ("sfmdata,d", po::value<std::string>(&sfmFilePath)->required(), "The sfm_data.json kind of file generated by OpenMVG [it could be also a bundle.out to use an older version of OpenMVG]")
      ("siftPath,s", po::value<std::string>(&descriptorsFolder), "Folder containing the .desc. If not provided, it will be assumed to be parent(sfmdata)/matches [for the older version of openMVG it is the list.txt]")
      ("algorithm,", po::value<std::string>(&algostring)->default_value(algostring), "Algorithm type: FirstBest=0, BestResult=1, AllResults=2, Cluster=3" )
      ("mediafile,m", po::value<std::string>(&mediaFilepath)->required(), "The folder path or the filename for the media to track")
      ("refineIntrinsics,", po::bool_switch(&param._refineIntrinsics), "Enable/Disable camera intrinsics refinement for each localized image")
      ("globalBundle,", po::bool_switch(&globalBundle), "If --refineIntrinsics is not set, this option allows to run a final global budndle adjustment to refine the scene")
#if HAVE_ALEMBIC
      ("export,e", po::value<std::string>(&exportFile)->default_value(exportFile), "Filename for the SfM_Data export file (where camera poses will be stored). Default : trackedcameras.json If Alambic is enable it will also export an .abc file of the scene with the same name")
#endif
#if HAVE_CCTAG
      ("useSIFT_CCTAG,", po::bool_switch(&useSIFT_CCTAG), "If provided, for each image it will extract both SIFT and the CCTAG.")
#endif
      ;

  po::variables_map vm;

  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if(vm.count("help") || (argc == 1))
    {
      POPART_COUT(desc);
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    POPART_CERR("ERROR: " << e.what() << std::endl);
    POPART_COUT("Usage:\n\n" << desc);
    return EXIT_FAILURE;
  }
  if(vm.count("algorithm"))
  {
    param._algorithm = localization::VoctreeLocalizer::initFromString(algostring);
  }
  if(vm.count("preset"))
  {
    param._featurePreset = features::describerPreset_stringToEnum(preset);
  }
  {
    // the bundle adjustment can be run for now only if the refine intrinsics option is not set
    globalBundle = (globalBundle && !param._refineIntrinsics);
    POPART_COUT("Program called with the following parameters:");
    POPART_COUT("\tvoctree: " << vocTreeFilepath);
    POPART_COUT("\tweights: " << weightsFilepath);
    POPART_COUT("\tcalibration: " << calibFile);
    POPART_COUT("\tsfmdata: " << sfmFilePath);
    POPART_COUT("\tmediafile: " << mediaFilepath);
    POPART_COUT("\tsiftPath: " << descriptorsFolder);
    POPART_COUT("\tresults: " << param._numResults);
    POPART_COUT("\tcommon views: " << param._numCommonViews);
    POPART_COUT("\trefineIntrinsics: " << param._refineIntrinsics);
    POPART_COUT("\tpreset: " << param._featurePreset);
    POPART_COUT("\tglobalBundle: " << globalBundle);
//    POPART_COUT("\tvisual debug: " << visualDebug);
    POPART_COUT("\talgorithm: " << param._algorithm);
  }
 
  // init the localizer
  localization::VoctreeLocalizer localizer(sfmFilePath,
                                           descriptorsFolder,
                                           vocTreeFilepath,
                                           weightsFilepath
#if HAVE_CCTAG
                                           , useSIFT_CCTAG
#endif
                                           );
  bool isInit = localizer.isInit();
  
  if(!isInit)
  {
    POPART_CERR("ERROR while initializing the localizer!");
    return EXIT_FAILURE;
  }
  
  // create the feedProvider
  dataio::FeedProvider feed(mediaFilepath, calibFile);
  if(!feed.isInit())
  {
    POPART_CERR("ERROR while initializing the FeedProvider!");
    return EXIT_FAILURE;
  }
  
#if HAVE_ALEMBIC
  dataio::AlembicExporter exporter( exportFile );
  exporter.addPoints(localizer.getSfMData().GetLandmarks());
#endif
  
  image::Image<unsigned char> imageGrey;
  cameras::Pinhole_Intrinsic_Radial_K3 queryIntrinsics;
  bool hasIntrinsics = false;
  
  size_t frameCounter = 0;
  std::string currentImgName;
  
  // Define an accumulator set for computing the mean and the
  // standard deviation of the time taken for localization
  bacc::accumulator_set<double, bacc::stats<bacc::tag::mean, bacc::tag::min, bacc::tag::max, bacc::tag::sum > > stats;
  
  std::vector<localization::LocalizationResult> localizationResults;
  
  while(feed.next(imageGrey, queryIntrinsics, currentImgName, hasIntrinsics))
  {
    POPART_COUT("******************************");
    POPART_COUT("FRAME " << myToString(frameCounter,4));
    POPART_COUT("******************************");
    localization::LocalizationResult locResult;
    std::vector<pair<IndexT, IndexT> > ids;
    auto detect_start = std::chrono::steady_clock::now();
    localizer.localize(imageGrey, 
                       &param,
                       hasIntrinsics /*useInputIntrinsics*/,
                       queryIntrinsics,
                       locResult);
    auto detect_end = std::chrono::steady_clock::now();
    auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
    POPART_COUT("\nLocalization took  " << detect_elapsed.count() << " [ms]");
    stats(detect_elapsed.count());
    
    // save data
    if(locResult.isValid())
    {
#if HAVE_ALEMBIC
      exporter.appendCamera("camera."+myToString(frameCounter,4), locResult.getPose(), &queryIntrinsics, mediaFilepath, frameCounter, frameCounter);
#endif
      if(globalBundle)
      {
        localizationResults.emplace_back(locResult);
      }
    }
    else
    {
#if HAVE_ALEMBIC
      // @fixme for now just add a fake camera so that it still can be see in MAYA
      exporter.appendCamera("camera.V."+myToString(frameCounter,4), geometry::Pose3(), &queryIntrinsics, mediaFilepath, frameCounter, frameCounter);
#endif
      POPART_CERR("Unable to localize frame " << frameCounter);
    }
    ++frameCounter;
  }
  
  if(globalBundle)
  {
    // run a bundle adjustment
    const bool BAresult = localization::refineSequence(&queryIntrinsics, localizationResults);
    if(!BAresult)
    {
      POPART_CERR("Bundle Adjustment failed!");
    }
    else
    {
#if HAVE_ALEMBIC
      // now copy back in a new abc with the same name file and BUNDLE appended at the end
      dataio::AlembicExporter exporterBA( bfs::path(exportFile).stem().string()+".BUNDLE.abc" );
      size_t idx = 0;
      for(const localization::LocalizationResult &res : localizationResults)
      {
        if(res.isValid())
        {
          assert(idx < localizationResults.size());
          exporterBA.appendCamera("camera."+myToString(idx,4), res.getPose(), &queryIntrinsics, mediaFilepath, frameCounter, frameCounter);
        }
        else
        {
          exporterBA.appendCamera("camera.V."+myToString(idx,4), geometry::Pose3(), &queryIntrinsics, mediaFilepath, frameCounter, frameCounter);
        }
        idx++;
      }
      exporterBA.addPoints(localizer.getSfMData().GetLandmarks());
#endif
    }
  }
  
  // print out some time stats
  POPART_COUT("\n\n******************************");
  POPART_COUT("Localized " << frameCounter << " images");
  POPART_COUT("Processing took " << bacc::sum(stats)/1000 << " [s] overall");
  POPART_COUT("Mean time for localization:   " << bacc::mean(stats) << " [ms]");
  POPART_COUT("Max time for localization:   " << bacc::max(stats) << " [ms]");
  POPART_COUT("Min time for localization:   " << bacc::min(stats) << " [ms]");
}
