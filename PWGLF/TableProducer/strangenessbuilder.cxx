// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//
//  *+-+*+-+*+-+*+-+*+-+*+-+*
//  Strangeness builder task
//  *+-+*+-+*+-+*+-+*+-+*+-+*
//
//  This task loops over a set of V0 and cascade indices and
//  creates the corresponding analysis tables that contain
//  the typical information required for analysis.
//
//  PERFORMANCE WARNING: this task includes several track
//  propagation calls that are intrinsically heavy. Please
//  also be cautious when adjusting selections: these can
//  increase / decrease CPU consumption quite significantly.
//
//  IDEAL USAGE: if you are interested in taking V0s and
//  cascades and propagating TrackParCovs based on these,
//  please do not re-propagate the daughters. Instead,
//  the tables generated by this builder task can be used
//  to instantiate a TrackPar object (default operation)
//  or even a TrackParCov object (for which you will
//  need to enable the option of producing the V0Cov and
//  CascCov tables too).
//
//    Comments, questions, complaints, suggestions?
//    Please write to:
//    david.dobrigkeit.chinellato@cern.ch
//

#include <cmath>
#include <array>
#include <cstdlib>
#include <map>
#include <iterator>
#include <utility>

#include "Framework/runDataProcessing.h"
#include "Framework/RunningWorkflowInfo.h"
#include "Framework/AnalysisTask.h"
#include "Framework/AnalysisDataModel.h"
#include "Framework/ASoAHelpers.h"
#include "DetectorsVertexing/DCAFitterN.h"
#include "ReconstructionDataFormats/Track.h"
#include "Common/Core/RecoDecay.h"
#include "Common/Core/trackUtilities.h"
#include "PWGLF/DataModel/LFStrangenessTables.h"
#include "Common/Core/TrackSelection.h"
#include "Common/DataModel/TrackSelectionTables.h"
#include "DetectorsBase/Propagator.h"
#include "DetectorsBase/GeometryManager.h"
#include "DataFormatsParameters/GRPObject.h"
#include "DataFormatsParameters/GRPMagField.h"
#include "CCDB/BasicCCDBManager.h"

#include "TFile.h"
#include "TH2F.h"
#include "TProfile.h"
#include "TLorentzVector.h"
#include "Math/Vector4D.h"
#include "TPDGCode.h"
#include "TDatabasePDG.h"

using namespace o2;
using namespace o2::framework;
using namespace o2::framework::expressions;
using std::array;

// Acts as std::multimap for cascades
namespace o2::aod
{
namespace v0tocascmap
{
DECLARE_SOA_ARRAY_INDEX_COLUMN(Cascade, cascadeCandidate);
} // namespace v0tocascmap
DECLARE_SOA_TABLE(V0ToCascMap, "AOD", "V0TOCASCMAP",
                  v0tocascmap::CascadeIds);

} // namespace o2::aod

// use parameters + cov mat non-propagated, aux info + (extension propagated)
using FullTracksExt = soa::Join<aod::Tracks, aod::TracksExtra, aod::TracksCov, aod::TracksDCA>;
using FullTracksExtIU = soa::Join<aod::TracksIU, aod::TracksExtra, aod::TracksCovIU, aod::TracksDCA>;
using LabeledTracks = soa::Join<aod::Tracks, aod::McTrackLabels>;
using V0WithCascadeRefs = soa::Join<aod::V0s, aod::V0ToCascMap>;

//*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
// Task to create array of bachelor track indices for single-for-loop processing
struct produceV0ToCascMap {
  Produces<aod::V0ToCascMap> v0toCascMap;
  std::vector<int> lCascadeArray;

  void process(aod::Collision const& collision, aod::Tracks const&, aod::V0s const& V0s, aod::Cascades const& cascades)
  {
    std::multimap<int, int> stdV0ToCascMap;
    typedef std::multimap<int, int>::iterator stdV0ToCascMapIter;
    for (auto& cascade : cascades) {
      stdV0ToCascMap.insert(std::pair<int, int>(cascade.v0().globalIndex(), cascade.globalIndex()));
    }
    for (auto& v0 : V0s) {
      std::pair<stdV0ToCascMapIter, stdV0ToCascMapIter> result = stdV0ToCascMap.equal_range(v0.globalIndex());
      lCascadeArray.clear();
      for (stdV0ToCascMapIter it = result.first; it != result.second; it++) {
        lCascadeArray.push_back(it->second);
      }
      // Populate with the std::vector, please
      v0toCascMap(lCascadeArray);
    }
  }
};

//*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
// Builder task: rebuilds strangeness candidates
// The prefilter part skims the list of good V0s to re-reconstruct so that
// CPU is saved in case there are specific selections that are to be done
struct strangenessBuilder {
  Produces<aod::StoredV0Datas> v0data;
  Produces<aod::CascData> cascdata;
  Produces<aod::V0Covs> v0covs;     // MC labels for cascades
  Produces<aod::CascCovs> casccovs; // MC labels for cascades
  Service<o2::ccdb::BasicCCDBManager> ccdb;

  // Configurables related to table creation
  Configurable<int> createCascades{"createCascades", -1, {"Produces cascade data. -1: auto, 0: don't, 1: yes. Default: auto (-1)"}};
  Configurable<int> createV0CovMats{"createV0CovMats", -1, {"Produces V0 cov matrices. -1: auto, 0: don't, 1: yes. Default: auto (-1)"}};
  Configurable<int> createCascCovMats{"createCascCovMats", -1, {"Produces V0 cov matrices. -1: auto, 0: don't, 1: yes. Default: auto (-1)"}};

  // Topological selection criteria
  Configurable<float> dcanegtopv{"dcanegtopv", .1, "DCA Neg To PV"};
  Configurable<float> dcapostopv{"dcapostopv", .1, "DCA Pos To PV"};
  Configurable<int> mincrossedrows{"mincrossedrows", 70, "min crossed rows"};
  Configurable<double> v0cospa{"v0cospa", 0.995, "V0 CosPA"}; // double -> N.B. dcos(x)/dx = 0 at x=0)
  Configurable<float> dcav0dau{"dcav0dau", 1.0, "DCA V0 Daughters"};
  Configurable<float> v0radius{"v0radius", 5.0, "v0radius"};
  Configurable<int> isRun2{"isRun2", 0, "if Run2: demand TPC refit"};

  // Configurables related to cascade building
  Configurable<float> dcabachtopv{"dcabachtopv", .05, "DCA Bach To PV"};
  Configurable<float> cascradius{"cascradius", 0.9, "cascradius"};
  Configurable<float> dcacascdau{"dcacascdau", 1.0, "DCA cascade Daughters"};
  Configurable<float> lambdaMassWindow{"lambdaMassWindow", .01, "Distance from Lambda mass"};

  // Operation and minimisation criteria
  Configurable<double> d_bz_input{"d_bz", -999, "bz field, -999 is automatic"};
  Configurable<bool> d_UseAbsDCA{"d_UseAbsDCA", true, "Use Abs DCAs"};
  Configurable<bool> d_UseWeightedPCA{"d_UseWeightedPCA", false, "Vertices use cov matrices"};
  Configurable<int> useMatCorrType{"useMatCorrType", 0, "0: none, 1: TGeo, 2: LUT"};
  Configurable<int> rejDiffCollTracks{"rejDiffCollTracks", 0, "rejDiffCollTracks"};

  // CCDB options
  Configurable<std::string> ccdburl{"ccdb-url", "http://alice-ccdb.cern.ch", "url of the ccdb repository"};
  Configurable<std::string> grpPath{"grpPath", "GLO/GRP/GRP", "Path of the grp file"};
  Configurable<std::string> grpmagPath{"grpmagPath", "GLO/Config/GRPMagField", "CCDB path of the GRPMagField object"};
  Configurable<std::string> lutPath{"lutPath", "GLO/Param/MatLUT", "Path of the Lut parametrization"};
  Configurable<std::string> geoPath{"geoPath", "GLO/Config/GeometryAligned", "Path of the geometry file"};

  int mRunNumber;
  float d_bz;
  float maxSnp;  // max sine phi for propagation
  float maxStep; // max step size (cm) for propagation
  o2::base::MatLayerCylSet* lut = nullptr;

  // Define o2 fitter, 2-prong, active memory (no need to redefine per event)
  o2::vertexing::DCAFitterN<2> fitter;

  // define positive and negative tracks in active memory (no need to reallocate)
  o2::track::TrackParCov lPositiveTrack;
  o2::track::TrackParCov lNegativeTrack;
  o2::track::TrackParCov lBachelorTrack;
  o2::track::TrackParCov lV0Track;
  o2::track::TrackParCov lCascadeTrack;

  // Helper struct to pass V0 information
  struct {
    int posTrackId;
    int negTrackId;
    int collisionId;
    int globalIndex;
    float posTrackX;
    float negTrackX;
    std::array<float, 3> pos;
    std::array<float, 3> posP;
    std::array<float, 3> negP;
    float dcaV0dau;
    float posDCAxy;
    float negDCAxy;
    float cosPA;
    float V0radius;
    float lambdaMass;
    float antilambdaMass;
  } v0candidate;

  // Helper struct to pass cascade information
  // N.B.: the V0 properties aren't needed
  // Processing will take place sequentially
  struct {
    int v0Id;
    int bachelorId;
    int collisionId;
    int charge;
    std::array<float, 3> pos;
    std::array<float, 3> bachP;
    float dcacascdau;
    float bachDCAxy;
    float cascradius;
  } cascadecandidate;

  HistogramRegistry registry{
    "registry",
    {{"hEventCounter", "hEventCounter", {HistType::kTH1F, {{1, 0.0f, 1.0f}}}},
     {"hCaughtExceptions", "hCaughtExceptions", {HistType::kTH1F, {{1, 0.0f, 1.0f}}}},
     {"hV0Criteria", "hV0Criteria", {HistType::kTH1F, {{10, 0.0f, 10.0f}}}},
     {"hCascadeCriteria", "hCascadeCriteria", {HistType::kTH1F, {{10, 0.0f, 10.0f}}}}}};

  void init(InitContext& context)
  {
    // using namespace analysis::lambdakzerobuilder;
    mRunNumber = 0;
    d_bz = 0;
    maxSnp = 0.85f;  // could be changed later
    maxStep = 2.00f; // could be changed later

    ccdb->setURL(ccdburl);
    ccdb->setCaching(true);
    ccdb->setLocalObjectValidityChecking();
    ccdb->setFatalWhenNull(false);

    lut = o2::base::MatLayerCylSet::rectifyPtrFromFile(ccdb->get<o2::base::MatLayerCylSet>(lutPath));
    if (!o2::base::GeometryManager::isGeometryLoaded()) {
      ccdb->get<TGeoManager>(geoPath);
    }

    if (doprocessRun2 == false && doprocessRun3 == false) {
      LOGF(fatal, "Neither processRun2 nor processRun3 enabled. Please choose one.");
    }
    if (doprocessRun2 == true && doprocessRun3 == true) {
      LOGF(fatal, "Cannot enable processRun2 and processRun3 at the same time. Please choose one.");
    }

    // Checking for subscriptions to:
    // - cascades
    // - covariance matrices
    auto& workflows = context.services().get<RunningWorkflowInfo const>();
    for (DeviceSpec const& device : workflows.devices) {
      for (auto const& input : device.inputs) {
        auto enable = [&input](const std::string tablename, Configurable<int>& flag) {
          const std::string table = tablename;
          if (input.matcher.binding == table) {
            if (flag < 0) {
              flag.value = 1;
              LOGF(info, "Auto-enabling table: %s", table.c_str());
            } else if (flag > 0) {
              flag.value = 1;
              LOGF(info, "Table %s already enabled", table.c_str());
            } else {
              LOGF(info, "Table %s disabled", table.c_str());
            }
          }
        };
        enable("CascData", createCascades);
        enable("V0Covs", createV0CovMats);
        enable("CascCovs", createCascCovMats);
      }
    }

    //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
    LOGF(info, "Strangeness builder configuration:");
    if (doprocessRun2 == true) {
      LOGF(info, "Run 2 processing enabled. Will subscribe to Tracks table.");
    };
    if (doprocessRun3 == true) {
      LOGF(info, "Run 3 processing enabled. Will subscribe to TracksIU table.");
    };
    if (createCascades > 0) {
      LOGF(info, "-> Will produce cascade data table");
    };
    if (createV0CovMats > 0) {
      LOGF(info, "-> Will produce V0 cov mat table");
    };
    if (createCascCovMats > 0) {
      LOGF(info, "-> Will produce cascade cov mat table");
    };
    //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*

    // initialize O2 2-prong fitter (only once)
    fitter.setPropagateToPCA(true);
    fitter.setMaxR(200.);
    fitter.setMinParamChange(1e-3);
    fitter.setMinRelChi2Change(0.9);
    fitter.setMaxDZIni(1e9);
    fitter.setMaxChi2(1e9);
    fitter.setUseAbsDCA(d_UseAbsDCA);
    fitter.setWeightedFinalPCA(d_UseWeightedPCA);

    // Material correction in the DCA fitter
    o2::base::Propagator::MatCorrType matCorr = o2::base::Propagator::MatCorrType::USEMatCorrNONE;
    if (useMatCorrType == 1)
      matCorr = o2::base::Propagator::MatCorrType::USEMatCorrTGeo;
    if (useMatCorrType == 2)
      matCorr = o2::base::Propagator::MatCorrType::USEMatCorrLUT;
    fitter.setMatCorrType(matCorr);
  }

  void initCCDB(aod::BCsWithTimestamps::iterator const& bc)
  {
    if (mRunNumber == bc.runNumber()) {
      return;
    }
    auto run3grp_timestamp = bc.timestamp();

    o2::parameters::GRPObject* grpo = ccdb->getForTimeStamp<o2::parameters::GRPObject>(grpPath, run3grp_timestamp);
    o2::parameters::GRPMagField* grpmag = 0x0;
    if (grpo) {
      o2::base::Propagator::initFieldFromGRP(grpo);
      if (d_bz_input < -990) {
        // Fetch magnetic field from ccdb for current collision
        d_bz = grpo->getNominalL3Field();
        LOG(info) << "Retrieved GRP for timestamp " << run3grp_timestamp << " with magnetic field of " << d_bz << " kZG";
      } else {
        d_bz = d_bz_input;
      }
    } else {
      grpmag = ccdb->getForTimeStamp<o2::parameters::GRPMagField>(grpmagPath, run3grp_timestamp);
      if (!grpmag) {
        LOG(fatal) << "Got nullptr from CCDB for path " << grpmagPath << " of object GRPMagField and " << grpPath << " of object GRPObject for timestamp " << run3grp_timestamp;
      }
      o2::base::Propagator::initFieldFromGRP(grpmag);
      if (d_bz_input < -990) {
        // Fetch magnetic field from ccdb for current collision
        d_bz = std::lround(5.f * grpmag->getL3Current() / 30000.f);
        LOG(info) << "Retrieved GRP for timestamp " << run3grp_timestamp << " with magnetic field of " << d_bz << " kZG";
      } else {
        d_bz = d_bz_input;
      }
    }
    o2::base::Propagator::Instance()->setMatLUT(lut);
    mRunNumber = bc.runNumber();
    // Set magnetic field value once known
    fitter.setBz(d_bz);
  }

  template <class TTracksTo>
  bool buildV0Candidate(aod::Collision const& collision, TTracksTo const& posTrack, TTracksTo const& negTrack, Bool_t lRun3 = kTRUE)
  {
    // value 0.5: any considered V0
    registry.fill(HIST("hV0Criteria"), 0.5);
    if (isRun2) {
      if (!(posTrack.trackType() & o2::aod::track::TPCrefit) && !lRun3) {
        return false;
      }
      if (!(negTrack.trackType() & o2::aod::track::TPCrefit) && !lRun3) {
        return false;
      }
    }
    // Passes TPC refit
    registry.fill(HIST("hV0Criteria"), 1.5);
    if (posTrack.tpcNClsCrossedRows() < mincrossedrows || negTrack.tpcNClsCrossedRows() < mincrossedrows) {
      return false;
    }
    // passes crossed rows
    registry.fill(HIST("hV0Criteria"), 2.5);
    if (fabs(posTrack.dcaXY()) < dcapostopv || fabs(negTrack.dcaXY()) < dcanegtopv) {
      return false;
    }
    // passes DCAxy
    registry.fill(HIST("hV0Criteria"), 3.5);

    // Change strangenessBuilder tracks
    lPositiveTrack = getTrackParCov(posTrack);
    lNegativeTrack = getTrackParCov(negTrack);

    //---/---/---/
    // Move close to minima
    int nCand = 0;
    try {
      nCand = fitter.process(lPositiveTrack, lNegativeTrack);
    } catch (...) {
      registry.fill(HIST("hCaughtExceptions"), 0.5f);
      LOG(error) << "Exception caught in DCA fitter process call!";
    }
    if (nCand == 0) {
      return false;
    }

    lPositiveTrack.getPxPyPzGlo(v0candidate.posP);
    lNegativeTrack.getPxPyPzGlo(v0candidate.negP);

    // get decay vertex coordinates
    const auto& vtx = fitter.getPCACandidate();
    for (int i = 0; i < 3; i++) {
      v0candidate.pos[i] = vtx[i];
    }

    v0candidate.dcaV0dau = TMath::Sqrt(fitter.getChi2AtPCACandidate());

    // Apply selections so a skimmed table is created only
    if (v0candidate.dcaV0dau > dcav0dau) {
      return false;
    }

    // Passes DCA between daughters check
    registry.fill(HIST("hV0Criteria"), 4.5);

    v0candidate.cosPA = RecoDecay::cpa(array{collision.posX(), collision.posY(), collision.posZ()}, array{v0candidate.pos[0], v0candidate.pos[1], v0candidate.pos[2]}, array{v0candidate.posP[0] + v0candidate.negP[0], v0candidate.posP[1] + v0candidate.negP[1], v0candidate.posP[2] + v0candidate.negP[2]});
    if (v0candidate.cosPA < v0cospa) {
      return false;
    }

    // Passes CosPA check
    registry.fill(HIST("hV0Criteria"), 5.5);

    v0candidate.V0radius = RecoDecay::sqrtSumOfSquares(v0candidate.pos[0], v0candidate.pos[1]);
    if (v0candidate.V0radius < v0radius) {
      return false;
    }

    // Passes radius check
    registry.fill(HIST("hV0Criteria"), 6.5);

    // store V0 track for a) cascade minimization and b) exporting for decay chains
    lV0Track = fitter.createParentTrackParCov();
    lV0Track.setAbsCharge(0); // just in case

    // Fill in lambda masses (necessary for cascades)
    v0candidate.lambdaMass = RecoDecay::m(array{v0candidate.posP, v0candidate.negP}, array{RecoDecay::getMassPDG(kProton), RecoDecay::getMassPDG(kPiPlus)});
    v0candidate.antilambdaMass = RecoDecay::m(array{v0candidate.posP, v0candidate.negP}, array{RecoDecay::getMassPDG(kPiPlus), RecoDecay::getMassPDG(kProton)});

    // Return OK: passed all v0 candidate selecton criteria
    return true;
  }

  template <class TTracksTo>
  bool buildCascadeCandidate(aod::Collision const& collision, TTracksTo const& bachTrack, Bool_t lRun3 = kTRUE)
  {
    // value 0.5: any considered cascade
    registry.fill(HIST("hCascadeCriteria"), 0.5);

    // bachelor DCA track to PV
    cascadecandidate.bachDCAxy = bachTrack.dcaXY();
    if (cascadecandidate.bachDCAxy < dcabachtopv)
      return false;
    registry.fill(HIST("hCascadeCriteria"), 1.5);

    // Overall cascade charge
    cascadecandidate.charge = bachTrack.signed1Pt() > 0 ? +1 : -1;

    // Better check than before: check also against charge
    // Should reduce unnecessary combinations
    if (cascadecandidate.charge < 0 && TMath::Abs(v0candidate.lambdaMass - 1.116) > lambdaMassWindow)
      return false;
    if (cascadecandidate.charge > 0 && TMath::Abs(v0candidate.antilambdaMass - 1.116) > lambdaMassWindow)
      return false;
    registry.fill(HIST("hCascadeCriteria"), 2.5);

    // Do actual minimization
    lBachelorTrack = getTrackParCov(bachTrack);
    auto nCand = fitter.process(lV0Track, lBachelorTrack);
    if (nCand == 0)
      return false;
    registry.fill(HIST("hCascadeCriteria"), 3.5);

    fitter.getTrack(1).getPxPyPzGlo(cascadecandidate.bachP);

    // get decay vertex coordinates
    const auto& vtx = fitter.getPCACandidate();
    for (int i = 0; i < 3; i++) {
      cascadecandidate.pos[i] = vtx[i];
    }

    // Cascade radius
    cascadecandidate.cascradius = RecoDecay::sqrtSumOfSquares(cascadecandidate.pos[0], cascadecandidate.pos[1]);
    if (cascadecandidate.cascradius < cascradius)
      return false;
    registry.fill(HIST("hCascadeCriteria"), 4.5);

    // DCA between cascade daughters
    cascadecandidate.dcacascdau = TMath::Sqrt(fitter.getChi2AtPCACandidate());
    if (cascadecandidate.cascradius < dcacascdau)
      return false;
    registry.fill(HIST("hCascadeCriteria"), 5.5);

    // store V0 track for a) cascade minimization and b) exporting for decay chains
    lCascadeTrack = fitter.createParentTrackParCov();
    lCascadeTrack.setAbsCharge(cascadecandidate.charge); // just in case

    return true;
  }

  template <class TTracksTo, typename TV0Objects>
  void buildStrangenessTables(aod::Collision const& collision, TV0Objects const& V0s, aod::Cascades const& cascades, TTracksTo const& tracks, Bool_t lRun3 = kTRUE)
  {
    registry.fill(HIST("hEventCounter"), 0.5);

    float V0CovMatrix[21];
    float CascCovMatrix[21];
    std::array<float, 21> stdCovV0 = {0.};
    std::array<float, 21> stdCovCasc = {0.};

    for (auto& V0 : V0s) {
      // Track preselection part
      auto posTrackCast = V0.template posTrack_as<TTracksTo>();
      auto negTrackCast = V0.template negTrack_as<TTracksTo>();

      // populates v0candidate struct declared inside strangenessbuilder
      bool validCandidate = buildV0Candidate(collision, posTrackCast, negTrackCast, lRun3);

      if (!validCandidate)
        continue; // doesn't pass selections

      // populates table for V0 analysis
      v0data(v0candidate.posTrackId,
             v0candidate.negTrackId,
             v0candidate.collisionId,
             v0candidate.globalIndex,
             v0candidate.posTrackX, v0candidate.negTrackX,
             v0candidate.pos[0], v0candidate.pos[1], v0candidate.pos[2],
             v0candidate.posP[0], v0candidate.posP[1], v0candidate.posP[2],
             v0candidate.negP[0], v0candidate.negP[1], v0candidate.negP[2],
             v0candidate.dcaV0dau,
             v0candidate.posDCAxy,
             v0candidate.negDCAxy);

      // populate V0 covariance matrices if required by any other task (experimental)
      if (createV0CovMats) {
        lV0Track.getCovXYZPxPyPzGlo(stdCovV0);
        for (Int_t iEl = 0; iEl < 21; iEl++) {
          V0CovMatrix[iEl] = stdCovV0[iEl];
        }
        v0covs(V0CovMatrix);
      }

      if (createCascades == 0)
        continue;
      auto lCascadeRefs = V0.cascadeCandidate();
      for (auto& cascade : lCascadeRefs) {
        auto bachTrackCast = cascade.template bachelor_as<TTracksTo>();
        bool validCascadeCandidate = buildCascadeCandidate(collision, bachTrackCast, lRun3);
        if (!validCascadeCandidate)
          continue; // doesn't pass cascade selections

        cascdata(V0.globalIndex(),
                 bachTrackCast.globalIndex(),
                 cascade.collisionId(),
                 cascadecandidate.charge,
                 cascadecandidate.pos[0], cascadecandidate.pos[1], cascadecandidate.pos[2],
                 v0candidate.pos[0], v0candidate.pos[1], v0candidate.pos[2],
                 v0candidate.posP[0], v0candidate.posP[1], v0candidate.posP[2],
                 v0candidate.negP[0], v0candidate.negP[1], v0candidate.negP[2],
                 cascadecandidate.bachP[0], cascadecandidate.bachP[1], cascadecandidate.bachP[2],
                 v0candidate.dcaV0dau, cascadecandidate.dcacascdau,
                 v0candidate.posDCAxy,
                 v0candidate.negDCAxy,
                 cascadecandidate.bachDCAxy);
        // populate casc covariance matrices if required by any other task (experimental)
        if (createCascCovMats) {
          lCascadeTrack.getCovXYZPxPyPzGlo(stdCovCasc);
          for (Int_t iEl = 0; iEl < 21; iEl++) {
            CascCovMatrix[iEl] = stdCovCasc[iEl];
          }
          casccovs(CascCovMatrix);
        }
      }
    }
  }

  void processRun2(aod::Collision const& collision, V0WithCascadeRefs const& V0s, aod::Cascades const& cascades, FullTracksExt const& tracks, aod::BCsWithTimestamps const&)
  {
    /* check the previous run number */
    auto bc = collision.bc_as<aod::BCsWithTimestamps>();
    initCCDB(bc);

    // do v0s, typecase correctly into tracks (Run 2 use case)
    buildStrangenessTables<FullTracksExt>(collision, V0s, cascades, tracks, kFALSE);
  }
  PROCESS_SWITCH(strangenessBuilder, processRun2, "Produce Run 2 V0 tables", true);

  void processRun3(aod::Collision const& collision, V0WithCascadeRefs const& V0s, aod::Cascades const& cascades, FullTracksExtIU const& tracks, aod::BCsWithTimestamps const&)
  {
    /* check the previous run number */
    auto bc = collision.bc_as<aod::BCsWithTimestamps>();
    initCCDB(bc);

    // do v0s, typecase correctly into tracksIU (Run 3 use case)
    buildStrangenessTables<FullTracksExtIU>(collision, V0s, cascades, tracks, kTRUE);
  }
  PROCESS_SWITCH(strangenessBuilder, processRun3, "Produce Run 3 V0 tables", false);
};

//*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
struct strangenessLabelBuilder {
  Produces<aod::McV0Labels> v0labels;     // MC labels for V0s
  Produces<aod::McCascLabels> casclabels; // MC labels for cascades
  // for bookkeeping purposes: how many V0s come from same mother etc

  void init(InitContext const&) {}

  void processDoNotBuildLabels(aod::Collisions::iterator const& collision)
  {
    // dummy process function - should not be required in the future
  }
  PROCESS_SWITCH(strangenessLabelBuilder, processDoNotBuildLabels, "Do not produce MC label tables", true);

  //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
  // build V0 labels if requested to do so
  void processBuildV0Labels(aod::Collision const& collision, aod::V0Datas const& v0table, LabeledTracks const&, aod::McParticles const& particlesMC)
  {
    for (auto& v0 : v0table) {
      int lLabel = -1;

      auto lNegTrack = v0.negTrack_as<LabeledTracks>();
      auto lPosTrack = v0.posTrack_as<LabeledTracks>();

      // Association check
      // There might be smarter ways of doing this in the future
      if (lNegTrack.has_mcParticle() && lPosTrack.has_mcParticle()) {
        auto lMCNegTrack = lNegTrack.mcParticle_as<aod::McParticles>();
        auto lMCPosTrack = lPosTrack.mcParticle_as<aod::McParticles>();
        if (lMCNegTrack.has_mothers() && lMCPosTrack.has_mothers()) {

          for (auto& lNegMother : lMCNegTrack.mothers_as<aod::McParticles>()) {
            for (auto& lPosMother : lMCPosTrack.mothers_as<aod::McParticles>()) {
              if (lNegMother.globalIndex() == lPosMother.globalIndex()) {
                lLabel = lNegMother.globalIndex();
              }
            }
          }
        }
      } // end association check
      // Construct label table (note: this will be joinable with V0Datas)
      v0labels(
        lLabel);
    }
  }
  PROCESS_SWITCH(strangenessLabelBuilder, processBuildV0Labels, "Produce V0 MC label tables", false);
  //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*

  //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
  // build cascade labels if requested to do so
  void processBuildCascadeLabels(aod::Collision const& collision, aod::CascDataExt const& casctable, aod::V0sLinked const&, aod::V0Datas const& v0table, LabeledTracks const&, aod::McParticles const&)
  {
    for (auto& casc : casctable) {
      // Loop over those that actually have the corresponding V0 associated to them
      auto v0 = casc.v0_as<o2::aod::V0sLinked>();
      if (!(v0.has_v0Data())) {
        continue; // skip those cascades for which V0 doesn't exist
      }
      auto v0data = v0.v0Data(); // de-reference index to correct v0data in case it exists
      int lLabel = -1;

      // Acquire all three daughter tracks, please
      auto lBachTrack = casc.bachelor_as<LabeledTracks>();
      auto lNegTrack = v0data.negTrack_as<LabeledTracks>();
      auto lPosTrack = v0data.posTrack_as<LabeledTracks>();

      // Association check
      // There might be smarter ways of doing this in the future
      if (lNegTrack.has_mcParticle() && lPosTrack.has_mcParticle() && lBachTrack.has_mcParticle()) {
        auto lMCBachTrack = lBachTrack.mcParticle_as<aod::McParticles>();
        auto lMCNegTrack = lNegTrack.mcParticle_as<aod::McParticles>();
        auto lMCPosTrack = lPosTrack.mcParticle_as<aod::McParticles>();

        // Step 1: check if the mother is the same, go up a level
        if (lMCNegTrack.has_mothers() && lMCPosTrack.has_mothers()) {
          for (auto& lNegMother : lMCNegTrack.mothers_as<aod::McParticles>()) {
            for (auto& lPosMother : lMCPosTrack.mothers_as<aod::McParticles>()) {
              if (lNegMother == lPosMother) {
                // if we got to this level, it means the mother particle exists and is the same
                // now we have to go one level up and compare to the bachelor mother too
                for (auto& lV0Mother : lNegMother.mothers_as<aod::McParticles>()) {
                  for (auto& lBachMother : lMCBachTrack.mothers_as<aod::McParticles>()) {
                    if (lV0Mother == lBachMother) {
                      lLabel = lV0Mother.globalIndex();
                    }
                  }
                } // end conditional V0-bach pair
              }   // end neg = pos mother conditional
            }
          } // end loop neg/pos mothers
        }   // end conditional of mothers existing
      }     // end association check
      // Construct label table (note: this will be joinable with CascDatas)
      casclabels(
        lLabel);
    } // end casctable loop
  }
  PROCESS_SWITCH(strangenessLabelBuilder, processBuildCascadeLabels, "Produce cascade MC label tables", false);
  //*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*+-+*
};

// Extends the v0data table with expression columns
struct lambdakzeroInitializer {
  Spawns<aod::V0Datas> v0datas;
  void init(InitContext const&) {}
};
/// Extends the cascdata table with expression columns
struct cascadeInitializer {
  Spawns<aod::CascDataExt> cascdataext;
  void init(InitContext const&) {}
};

WorkflowSpec defineDataProcessing(ConfigContext const& cfgc)
{
  return WorkflowSpec{
    adaptAnalysisTask<produceV0ToCascMap>(cfgc),
    adaptAnalysisTask<strangenessBuilder>(cfgc),
    adaptAnalysisTask<strangenessLabelBuilder>(cfgc),
    adaptAnalysisTask<lambdakzeroInitializer>(cfgc),
    adaptAnalysisTask<cascadeInitializer>(cfgc)};
}
