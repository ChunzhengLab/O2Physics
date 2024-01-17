// Copyright 2019-2024 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>

#include "Framework/runDataProcessing.h"
#include "Framework/AnalysisTask.h"
#include "Framework/AnalysisDataModel.h"
#include "Framework/ASoA.h"
#include "Framework/HistogramRegistry.h"

#include "Common/DataModel/EventSelection.h"
#include "Common/DataModel/Centrality.h"

#include "EMCALBase/Geometry.h"
#include "PWGJE/DataModel/EMCALClusters.h"
#include "PWGJE/DataModel/EMCALMatchedCollisions.h"
#include "DataFormatsEMCAL/Cell.h"
#include "DataFormatsEMCAL/Constants.h"
#include "DataFormatsEMCAL/AnalysisCluster.h"

#include "CommonDataFormat/InteractionRecord.h"

#include "TLorentzVector.h"
#include "TVector3.h"

/// \brief Simple pi0 reconstruction task used to scale the cell energy based on the difference in mass position in data and MC
/// \author Nicolas Strangmann <nicolas.strangmann@cern.ch>, Goethe University Frankfurt / Oak Ridge National Laoratory
/// \author Joshua Koenig <joshua.konig@cern.ch>, Goethe University Frankfurt
/// \since 12.01.2024
///
/// The task distiguishes the regions of EMCal, DCal and the 1/3rd modules and within these it differentiated between
/// the region on the edge, the region behind the TRD support structure and the rest (inner region)

using namespace o2::framework;
using namespace o2::framework::expressions;
using collisionEvSelIt = o2::aod::Collision;
using selectedClusters = o2::soa::Filtered<o2::aod::EMCALClusters>;
using selectedCluster = o2::soa::Filtered<o2::aod::EMCALCluster>;

o2::emcal::Geometry* gEMCalGeometry = nullptr;
const int NAcceptanceCategories = 10;
enum Acceptance_Category {
  kFullAcceptance = 0,
  kEMCalbehindTRD = 1,
  kEMCalBorder = 2,
  kEMCalInside = 3,
  kDCalbehindTRD = 4,
  kDCalBorder = 5,
  kDCalInside = 6,
  kOneThirdbehindTRD = 7,
  kOneThirdBorder = 8,
  kOneThirdInside = 9
};

// Columns behind the TRD support structure to investigate the scaling factor seperately for this region
std::array<int, 20> TRDColumns = {5, 6, 7, 8, 9, 34, 35, 36, 37, 38, 59, 60, 61, 62, 63, 87, 88, 89, 90, 91};

// Returns a boolean on whether a given EMCal column is behind the TRD support structure
bool IsBehindTRD(int col)
{
  return std::find(std::begin(TRDColumns), std::end(TRDColumns), col) != std::end(TRDColumns);
}

// Returns a boolean on whether a given EMCal row is on the border of a given supermodule
bool IsAtBorder(int row, bool smallmodule)
{
  return (row == 0 || (smallmodule && row == 7) || (!smallmodule && row == 23));
}

// Return one of nine acceptance categories and set the second and third parameter to the global row and column
int GetAcceptanceCategory(int cellid, int& globalrow, int& globalcol)
{
  auto [supermodule, module, phiInModule, etaInModule] = gEMCalGeometry->GetCellIndex(cellid);
  auto [row, col] = gEMCalGeometry->GetCellPhiEtaIndexInSModule(supermodule, module, phiInModule, etaInModule);

  // Calculate offset of global rows and columns
  int xoffset = supermodule % 2 * 48;
  int yoffset = supermodule / 2 * 24;

  if (supermodule > 11 && supermodule < 18) {
    xoffset = supermodule % 2 * 64;
  }
  if (supermodule > 11) {
    yoffset = (supermodule - 12) / 2 * 24 + (5 * 24 + 8);
  }

  // Add the offset to the local column and row
  globalcol = col + xoffset;
  globalrow = row + yoffset;

  // Add 48 columns for all odd supermodules and 16 for uneven DCal supermodules
  if (supermodule % 2) {
    col += 48;
    if (supermodule > 11 && supermodule < 18) {
      col += 16;
    }
  }

  // LOGF(info, "cellid: %d\tsupermodule: %d\tmodule: %d\tphiInModule: %d\tetaInModule: %d\trow: %d\tcol: %d", cellid, supermodule, module, phiInModule, etaInModule, row, col);

  if (supermodule >= 0 && supermodule <= 9) {
    if (IsBehindTRD(col)) {
      return kEMCalbehindTRD;
    } else if (IsAtBorder(row, false)) {
      return kEMCalBorder;
    } else {
      return kEMCalInside;
    }
  } else if (supermodule >= 12 && supermodule <= 17) {
    if (IsBehindTRD(col)) {
      return kDCalbehindTRD;
    } else if (IsAtBorder(row, false)) {
      return kDCalBorder;
    } else {
      return kDCalInside;
    }
  } else if (supermodule == 10 || supermodule == 11 || supermodule == 18 || supermodule == 19) {
    if (IsBehindTRD(col)) {
      return kOneThirdbehindTRD;
    } else if (IsAtBorder(row, true)) {
      return kOneThirdBorder;
    } else {
      return kOneThirdInside;
    }
  } else {
    LOGF(error, Form("Supermodule %d not found", supermodule));
    return 0;
  }
}

struct Photon {
  Photon(float eta_tmp, float phi_tmp, float energy_tmp, int clusteridin = 0, int cellidin = 0)
  {
    eta = eta_tmp;
    phi = phi_tmp;
    energy = energy_tmp;
    theta = 2 * std::atan2(std::exp(-eta), 1);
    px = energy * std::sin(theta) * std::cos(phi);
    py = energy * std::sin(theta) * std::sin(phi);
    pz = energy * std::cos(theta);
    pt = std::sqrt(px * px + py * py);
    photon.SetPxPyPzE(px, py, pz, energy);
    clusterid = clusteridin;
    cellid = cellidin;

    acceptance_category = GetAcceptanceCategory(cellid, row, col);
  }

  TLorentzVector photon;
  float pt;
  float px;
  float py;
  float pz;
  float eta;
  float phi;
  float energy;
  float theta;
  int row;                 // Global row
  int col;                 // Global column
  int acceptance_category; // One of the nine acceptance categories (EMCal, DCal or one third and behindTRD, border and inside)
  int cellid;
  int clusterid;
};

struct Meson {
  Meson(Photon p1, Photon p2) : pgamma1(p1),
                                pgamma2(p2)
  {
    pMeson = p1.photon + p2.photon;
  }
  Photon pgamma1;
  Photon pgamma2;
  TLorentzVector pMeson;

  float getMass() const { return pMeson.M(); }
  float getPt() const { return pMeson.Pt(); }
  float getOpeningAngle() const { return pgamma1.photon.Angle(pgamma2.photon.Vect()); }
};

struct Pi0EnergyScaleCalibTask {
  HistogramRegistry mHistManager{"NeutralMesonHistograms"};
  Preslice<o2::aod::EMCALClusterCells> perCluster = o2::aod::emcalclustercell::emcalclusterId;

  // configurable parameters
  Configurable<bool> mDoEventSel{"doEventSel", 0, "demand kINT7"};
  Configurable<double> mVertexCut{"vertexCut", -1, "apply z-vertex cut with value in cm"};
  Configurable<int> mTimeMin{"TimeMinCut", -600, "apply min timing cut (in ns)"};
  Configurable<int> mTimeMax{"TimeMaxCut", 900, "apply min timing cut (in ns)"};
  Configurable<float> mClusterMinM02Cut{"MinM02Cut", 0.1, "apply min M02 cut"};
  Configurable<float> mClusterMaxM02Cut{"MaxM02Cut", 0.7, "apply max M02 cut"};
  Configurable<float> mMinEnergyCut{"MinEnergyCut", 0.7, "apply min cluster energy cut"};
  Configurable<int> mMinNCellsCut{"MinNCellsCut", 1, "apply min cluster number of cell cut"};
  Configurable<float> mMinOpenAngleCut{"OpeningAngleCut", 0.0202, "apply min opening angle cut"};
  Configurable<std::string> mClusterDefinition{"clusterDefinition", "kV3Default", "cluster definition to be selected, e.g. V3Default"};
  Configurable<bool> mRequireBothPhotonsFromAcceptance{"RequireBothPhotonsFromAcceptance", 0, "Require both photons to be from the same acceptance category"};

  ConfigurableAxis pTBinning{"pTBinning", {200, 0.0f, 20.0f}, "Binning used along pT axis for inv mass histograms"};
  ConfigurableAxis invmassBinning{"invmassBinning", {200, 0.0f, 0.4f}, "Binning used for inv mass axis in inv mass - pT histograms"};

  // define cluster filter. It selects only those clusters which are of the type
  // specified in the string mClusterDefinition,e.g. kV3Default, which is V3 clusterizer with default
  // clusterization parameters
  o2::aod::EMCALClusterDefinition clusDef = o2::aod::emcalcluster::getClusterDefinitionFromString(mClusterDefinition.value);
  Filter clusterDefinitionSelection = o2::aod::emcalcluster::definition == static_cast<int>(clusDef);

  // define container for photons
  std::vector<Photon> mPhotons;

  /// \brief Create output histograms and initialize geometry
  void init(InitContext const&)
  {
    // create histograms
    using o2HistType = HistType;
    using o2Axis = AxisSpec;

    // load geometry used to match a cellid to a supermodule and the row+column
    gEMCalGeometry = o2::emcal::Geometry::GetInstanceFromRunNumber(300000);

    // create common axes
    const o2Axis bcAxis{3501, -0.5, 3500.5};
    const o2Axis AccCategoryAxis{10, -0.5, 9.5};
    const o2Axis RowAxis{208, -0.5, 207.5};
    const o2Axis ColAxis{96, -0.5, 95.5};

    mHistManager.add("events", "events;;#it{count}", o2HistType::kTH1F, {{4, 0.5, 4.5}});
    auto heventType = mHistManager.get<TH1>(HIST("events"));
    heventType->GetXaxis()->SetBinLabel(1, "All events");
    heventType->GetXaxis()->SetBinLabel(2, "One collision in BC");
    heventType->GetXaxis()->SetBinLabel(3, "Triggered");
    heventType->GetXaxis()->SetBinLabel(4, "Selected");
    mHistManager.add("eventBCAll", "Bunch crossing ID of event (all events)", o2HistType::kTH1F, {bcAxis});
    mHistManager.add("eventBCSelected", "Bunch crossing ID of event (selected events)", o2HistType::kTH1F, {bcAxis});
    mHistManager.add("eventVertexZAll", "z-vertex of event (all events)", o2HistType::kTH1F, {{200, -20, 20}});
    mHistManager.add("eventVertexZSelected", "z-vertex of event (selected events)", o2HistType::kTH1F, {{200, -20, 20}});

    // cluster properties
    for (bool iBeforeCuts : {false, true}) {
      const char* ClusterDirectory = iBeforeCuts ? "ClustersBeforeCuts" : "ClustersAfterCuts";
      mHistManager.add(Form("%s/clusterE", ClusterDirectory), "Energy of cluster", o2HistType::kTH1F, {{400, 0, 100, "#it{E} (GeV)"}});
      mHistManager.add(Form("%s/clusterTime", ClusterDirectory), "Time of cluster", o2HistType::kTH1F, {{500, -250, 250, "#it{t}_{cls} (ns)"}});
      mHistManager.add(Form("%s/clusterEtaPhi", ClusterDirectory), "Eta and phi of cluster", o2HistType::kTH3F, {{200, -1, 1, "#eta"}, {200, 0, 2 * TMath::Pi(), "#phi"}, AccCategoryAxis});
      mHistManager.add(Form("%s/clusterEtaPhiVsRow", ClusterDirectory), "Eta and phi of cluster", o2HistType::kTH3F, {{200, -1, 1, "#eta"}, {200, 0, 2 * TMath::Pi(), "#phi"}, RowAxis});
      mHistManager.add(Form("%s/clusterEtaPhiVsCol", ClusterDirectory), "Eta and phi of cluster", o2HistType::kTH3F, {{200, -1, 1, "#eta"}, {200, 0, 2 * TMath::Pi(), "#phi"}, ColAxis});
      mHistManager.add(Form("%s/clusterM02", ClusterDirectory), "M02 of cluster", o2HistType::kTH1F, {{400, 0, 5, "#it{M}_{02}"}});
      mHistManager.add(Form("%s/clusterM20", ClusterDirectory), "M20 of cluster", o2HistType::kTH1F, {{400, 0, 2.5, "#it{M}_{20}"}});
      mHistManager.add(Form("%s/clusterNLM", ClusterDirectory), "Number of local maxima of cluster", o2HistType::kTH1I, {{10, 0, 10, "#it{N}_{local maxima}"}});
      mHistManager.add(Form("%s/clusterNCells", ClusterDirectory), "Number of cells in cluster", o2HistType::kTH1I, {{50, 0, 50, "#it{N}_{cells}"}});
      mHistManager.add(Form("%s/clusterDistanceToBadChannel", ClusterDirectory), "Distance to bad channel", o2HistType::kTH1F, {{100, 0, 100, "#it{d}"}});
    }

    mHistManager.add("invMassVsPtVsAcc", "invariant mass and pT of meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, AccCategoryAxis});
    mHistManager.add("invMassVsPtVsAccBackground", "invariant mass and pT of background meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, AccCategoryAxis});
    mHistManager.add("invMassVsPtVsRow", "invariant mass and pT of meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, RowAxis});
    mHistManager.add("invMassVsPtVsRowBackground", "invariant mass and pT of background meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, RowAxis});
    mHistManager.add("invMassVsPtVsCol", "invariant mass and pT of background meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, ColAxis});
    mHistManager.add("invMassVsPtVsColBackground", "invariant mass and pT of background meson candidates", o2HistType::kTH3F, {invmassBinning, pTBinning, ColAxis});
    initCategoryAxis(mHistManager.get<TH3>(HIST("invMassVsPtVsAcc")).get());
    initCategoryAxis(mHistManager.get<TH3>(HIST("invMassVsPtVsAccBackground")).get());
    initCategoryAxis(mHistManager.get<TH3>(HIST("ClustersBeforeCuts/clusterEtaPhi")).get());
    initCategoryAxis(mHistManager.get<TH3>(HIST("ClustersAfterCuts/clusterEtaPhi")).get());
  }

  /// \brief Process EMCAL clusters that are matched to a collisions
  void process(o2::soa::Join<o2::aod::Collisions, o2::aod::EvSels, o2::aod::EMCALMatchedCollisions>::iterator const& collision, o2::aod::Calos const& allcalos, selectedClusters const& clusters, o2::aod::EMCALClusterCells const& cells)
  {
    mHistManager.fill(HIST("events"), 1); // Fill "All events" bin of event histogram
    LOG(debug) << "processCollisions";

    if (collision.ambiguous()) { // Skip ambiguous collisions (those that are in BCs including multiple collisions)
      LOG(debug) << "Event not selected becaus there are multiple collisions in this BC, skipping";
      return;
    }
    mHistManager.fill(HIST("events"), 2); // Fill "One collision in BC" bin of event histogram

    if (mDoEventSel && (!collision.alias_bit(kTVXinEMC))) {
      LOG(debug) << "Event not selected becaus it is not kTVXinEMC, skipping";
      return;
    }
    mHistManager.fill(HIST("events"), 3); // Fill "Triggered" bin of event histogram
    mHistManager.fill(HIST("eventVertexZAll"), collision.posZ());
    if (mVertexCut > 0 && std::abs(collision.posZ()) > mVertexCut) {
      LOG(debug) << "Event not selected because of z-vertex cut z= " << collision.posZ() << " > " << mVertexCut << " cm, skipping";
      return;
    }
    mHistManager.fill(HIST("events"), 4); // Fill "Selected" bin of event histogram
    mHistManager.fill(HIST("eventVertexZSelected"), collision.posZ());

    ProcessClusters(clusters, cells);
    ProcessMesons();
  }

  template <typename Cluster>
  int GetLeadingCellID(Cluster const& cluster, o2::aod::EMCALClusterCells const& cells)
  {
    auto cellsofcluster = cells.sliceBy(perCluster, cluster.globalIndex());
    double maxamp = 0;
    int cellid = -1;
    for (const auto& cell : cellsofcluster) {
      if (cell.calo().amplitude() > maxamp) {
        maxamp = cell.calo().amplitude();
        cellid = cell.calo().cellNumber();
      }
    }
    return cellid;
  }

  /// \brief Process EMCAL clusters that are matched to a collisions
  template <typename Clusters>
  void ProcessClusters(Clusters const& clusters, o2::aod::EMCALClusterCells const& cells)
  {
    // clear photon vector
    mPhotons.clear();

    int globalCollID = -1000;

    // loop over all clusters from accepted collision
    // auto eventClusters = clusters.select(o2::aod::emcalcluster::bcId == theCollision.bc().globalBC());
    for (const auto& cluster : clusters) {

      auto collID = cluster.collisionId();
      if (globalCollID == -1000)
        globalCollID = collID;

      int cellid = GetLeadingCellID(cluster, cells);

      FillClusterQAHistos<decltype(cluster), 0>(cluster, cellid);

      if (ClusterRejectedByCut(cluster)) {
        continue;
      }

      FillClusterQAHistos<decltype(cluster), 1>(cluster, cellid);

      // put clusters in photon vector
      mPhotons.push_back(Photon(cluster.eta(), cluster.phi(), cluster.energy(), cluster.id(), cellid));
    }
  }

  /// \brief Fills the standard QA histograms for a given cluster
  template <typename Cluster, int BeforeCuts>
  void FillClusterQAHistos(Cluster const& cluster, int cellid)
  {
    // In this implementation the cluster properties are directly loaded from the flat table,
    // in the future one should consider using the AnalysisCluster object to work with after loading.
    static constexpr std::string_view clusterQAHistEnergy[2] = {"ClustersBeforeCuts/clusterE", "ClustersAfterCuts/clusterE"};
    static constexpr std::string_view clusterQAHistTime[2] = {"ClustersBeforeCuts/clusterTime", "ClustersAfterCuts/clusterTime"};
    static constexpr std::string_view clusterQAHistEtaPhi[2] = {"ClustersBeforeCuts/clusterEtaPhi", "ClustersAfterCuts/clusterEtaPhi"};
    static constexpr std::string_view clusterQAHistEtaPhiRow[2] = {"ClustersBeforeCuts/clusterEtaPhiVsRow", "ClustersAfterCuts/clusterEtaPhiVsRow"};
    static constexpr std::string_view clusterQAHistEtaPhiCol[2] = {"ClustersBeforeCuts/clusterEtaPhiVsCol", "ClustersAfterCuts/clusterEtaPhiVsCol"};
    static constexpr std::string_view clusterQAHistM02[2] = {"ClustersBeforeCuts/clusterM02", "ClustersAfterCuts/clusterM02"};
    static constexpr std::string_view clusterQAHistM20[2] = {"ClustersBeforeCuts/clusterM20", "ClustersAfterCuts/clusterM20"};
    static constexpr std::string_view clusterQAHistNLM[2] = {"ClustersBeforeCuts/clusterNLM", "ClustersAfterCuts/clusterNLM"};
    static constexpr std::string_view clusterQAHistNCells[2] = {"ClustersBeforeCuts/clusterNCells", "ClustersAfterCuts/clusterNCells"};
    static constexpr std::string_view clusterQAHistDistanceToBadChannel[2] = {"ClustersBeforeCuts/clusterDistanceToBadChannel", "ClustersAfterCuts/clusterDistanceToBadChannel"};
    mHistManager.fill(HIST(clusterQAHistEnergy[BeforeCuts]), cluster.energy());
    mHistManager.fill(HIST(clusterQAHistTime[BeforeCuts]), cluster.time());
    mHistManager.fill(HIST(clusterQAHistEtaPhi[BeforeCuts]), cluster.eta(), cluster.phi(), 0);
    int row, col = 0; // Initialize row and column, which are set in GetAcceptanceCategory and then used to fill the eta phi map
    mHistManager.fill(HIST(clusterQAHistEtaPhi[BeforeCuts]), cluster.eta(), cluster.phi(), GetAcceptanceCategory(cellid, row, col));
    mHistManager.fill(HIST(clusterQAHistEtaPhiRow[BeforeCuts]), cluster.eta(), cluster.phi(), row);
    mHistManager.fill(HIST(clusterQAHistEtaPhiCol[BeforeCuts]), cluster.eta(), cluster.phi(), col);
    mHistManager.fill(HIST(clusterQAHistM02[BeforeCuts]), cluster.m02());
    mHistManager.fill(HIST(clusterQAHistM20[BeforeCuts]), cluster.m20());
    mHistManager.fill(HIST(clusterQAHistNLM[BeforeCuts]), cluster.nlm());
    mHistManager.fill(HIST(clusterQAHistNCells[BeforeCuts]), cluster.nCells());
    mHistManager.fill(HIST(clusterQAHistDistanceToBadChannel[BeforeCuts]), cluster.distanceToBadChannel());
  }

  /// \brief Return a boolean that states, whether a cluster should be rejected by the applied cluster cuts
  template <typename Cluster>
  bool ClusterRejectedByCut(Cluster const& cluster)
  {
    // apply basic cluster cuts
    if (cluster.energy() < mMinEnergyCut) {
      LOG(debug) << "Cluster rejected because of energy cut";
      return true;
    }
    if (cluster.nCells() < mMinNCellsCut) {
      LOG(debug) << "Cluster rejected because of nCells cut";
      return true;
    }
    // Only apply M02 cut when cluster contains more than one cell
    if (cluster.nCells() > 1) {
      if (cluster.m02() < mClusterMinM02Cut || cluster.m02() > mClusterMaxM02Cut) {
        LOG(debug) << "Cluster rejected because of m02 cut";
        return true;
      }
    }
    if (cluster.time() < mTimeMin || cluster.time() > mTimeMax) {
      LOG(debug) << "Cluster rejected because of time cut";
      return true;
    }
    return false;
  }

  /// \brief Process meson candidates, calculate invariant mass and pT and fill histograms
  void ProcessMesons()
  {
    // if less then 2 clusters are found, skip event
    if (mPhotons.size() < 2)
      return;

    // loop over all photon combinations and build meson candidates
    for (unsigned int ig1 = 0; ig1 < mPhotons.size(); ++ig1) {
      for (unsigned int ig2 = ig1 + 1; ig2 < mPhotons.size(); ++ig2) {
        Meson meson(mPhotons[ig1], mPhotons[ig2]); // build meson from photons
        if (meson.getOpeningAngle() > mMinOpenAngleCut) {
          mHistManager.fill(HIST("invMassVsPtVsAcc"), meson.getMass(), meson.getPt(), 0);
          mHistManager.fill(HIST("invMassVsPtVsRow"), meson.getMass(), meson.getPt(), mPhotons[ig1].row);
          mHistManager.fill(HIST("invMassVsPtVsRow"), meson.getMass(), meson.getPt(), mPhotons[ig2].row);
          mHistManager.fill(HIST("invMassVsPtVsCol"), meson.getMass(), meson.getPt(), mPhotons[ig1].col);
          mHistManager.fill(HIST("invMassVsPtVsCol"), meson.getMass(), meson.getPt(), mPhotons[ig2].col);
          for (int iAcceptanceCategory = 1; iAcceptanceCategory < NAcceptanceCategories; iAcceptanceCategory++) {
            if ((!mRequireBothPhotonsFromAcceptance && (mPhotons[ig1].acceptance_category == iAcceptanceCategory || mPhotons[ig2].acceptance_category == iAcceptanceCategory)) ||
                (mPhotons[ig1].acceptance_category == iAcceptanceCategory && mPhotons[ig2].acceptance_category == iAcceptanceCategory)) {
              mHistManager.fill(HIST("invMassVsPtVsAcc"), meson.getMass(), meson.getPt(), iAcceptanceCategory);
            }
          }
          CalculateBackground(meson, ig1, ig2); // calculate background candidates (rotation background)
        }
      }
    }
  }

  /// \brief Calculate background (using rotation background method)
  void CalculateBackground(const Meson& meson, unsigned int ig1, unsigned int ig2)
  {
    // if less than 3 clusters are present, skip event
    if (mPhotons.size() < 3) {
      return;
    }
    const double rotationAngle = M_PI / 2.0; // 0.78539816339; // rotaion angle 90°

    TLorentzVector lvRotationPhoton1; // photon candidates which get rotated
    TLorentzVector lvRotationPhoton2; // photon candidates which get rotated
    TVector3 lvRotationPion;          // rotation axis
    for (unsigned int ig3 = 0; ig3 < mPhotons.size(); ++ig3) {
      // continue if photons are identical
      if (ig3 == ig1 || ig3 == ig2) {
        continue;
      }
      // calculate rotation axis
      lvRotationPion = (meson.pMeson).Vect();

      // initialize photons for rotation
      lvRotationPhoton1.SetPxPyPzE(mPhotons[ig1].px, mPhotons[ig1].py, mPhotons[ig1].pz, mPhotons[ig1].energy);
      lvRotationPhoton2.SetPxPyPzE(mPhotons[ig2].px, mPhotons[ig2].py, mPhotons[ig2].pz, mPhotons[ig2].energy);

      // rotate photons around rotation axis
      lvRotationPhoton1.Rotate(rotationAngle, lvRotationPion);
      lvRotationPhoton2.Rotate(rotationAngle, lvRotationPion);

      // initialize Photon objects for rotated photons
      Photon rotPhoton1(lvRotationPhoton1.Eta(), lvRotationPhoton1.Phi(), lvRotationPhoton1.E(), mPhotons[ig1].clusterid, mPhotons[ig1].cellid);
      Photon rotPhoton2(lvRotationPhoton2.Eta(), lvRotationPhoton2.Phi(), lvRotationPhoton2.E(), mPhotons[ig2].clusterid, mPhotons[ig2].cellid);

      // build meson from rotated photons
      Meson mesonRotated1(rotPhoton1, mPhotons[ig3]);
      Meson mesonRotated2(rotPhoton2, mPhotons[ig3]);

      // Fill histograms
      if (mesonRotated1.getOpeningAngle() > mMinOpenAngleCut) {
        mHistManager.fill(HIST("invMassVsPtVsAccBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), 0);
        mHistManager.fill(HIST("invMassVsPtVsRowBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), mPhotons[ig3].row);
        mHistManager.fill(HIST("invMassVsPtVsRowBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), rotPhoton1.row);
        mHistManager.fill(HIST("invMassVsPtVsColBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), mPhotons[ig3].col);
        mHistManager.fill(HIST("invMassVsPtVsColBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), rotPhoton1.col);
        for (int iAcceptanceCategory = 1; iAcceptanceCategory < NAcceptanceCategories; iAcceptanceCategory++) {
          if ((!mRequireBothPhotonsFromAcceptance && (rotPhoton1.acceptance_category == iAcceptanceCategory || mPhotons[ig3].acceptance_category == iAcceptanceCategory)) ||
              (rotPhoton1.acceptance_category == iAcceptanceCategory && mPhotons[ig3].acceptance_category == iAcceptanceCategory)) {
            mHistManager.fill(HIST("invMassVsPtVsAccBackground"), mesonRotated1.getMass(), mesonRotated1.getPt(), iAcceptanceCategory);
          }
        }
      }
      if (mesonRotated2.getOpeningAngle() > mMinOpenAngleCut) {
        mHistManager.fill(HIST("invMassVsPtVsAccBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), 0);
        mHistManager.fill(HIST("invMassVsPtVsRowBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), mPhotons[ig3].row);
        mHistManager.fill(HIST("invMassVsPtVsRowBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), rotPhoton2.row);
        mHistManager.fill(HIST("invMassVsPtVsColBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), mPhotons[ig3].col);
        mHistManager.fill(HIST("invMassVsPtVsColBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), rotPhoton2.col);
        for (int iAcceptanceCategory = 1; iAcceptanceCategory < NAcceptanceCategories; iAcceptanceCategory++) {
          if ((!mRequireBothPhotonsFromAcceptance && (rotPhoton2.acceptance_category == iAcceptanceCategory || mPhotons[ig3].acceptance_category == iAcceptanceCategory)) ||
              (rotPhoton2.acceptance_category == iAcceptanceCategory && mPhotons[ig3].acceptance_category == iAcceptanceCategory)) {
            mHistManager.fill(HIST("invMassVsPtVsAccBackground"), mesonRotated2.getMass(), mesonRotated2.getPt(), iAcceptanceCategory);
          }
        }
      }
    }
  }

  // Beautify acceptance category bin labels
  void initCategoryAxis(TH3* hist)
  {
    hist->GetZaxis()->SetTitle("Acceptance Category");
    hist->GetZaxis()->SetBinLabel(1, "FullAcceptance");
    hist->GetZaxis()->SetBinLabel(2, "EMCalbehindTRD");
    hist->GetZaxis()->SetBinLabel(3, "EMCalBorder");
    hist->GetZaxis()->SetBinLabel(4, "EMCalInside");
    hist->GetZaxis()->SetBinLabel(5, "DCalbehindTRD");
    hist->GetZaxis()->SetBinLabel(6, "DCalBorder");
    hist->GetZaxis()->SetBinLabel(7, "DCalInside");
    hist->GetZaxis()->SetBinLabel(8, "OneThirdbehindTRD");
    hist->GetZaxis()->SetBinLabel(9, "OneThirdBorder");
    hist->GetZaxis()->SetBinLabel(10, "OneThirdInside");
  }
};

WorkflowSpec defineDataProcessing(o2::framework::ConfigContext const& cfgc)
{
  return WorkflowSpec{
    adaptAnalysisTask<Pi0EnergyScaleCalibTask>(cfgc)};
}