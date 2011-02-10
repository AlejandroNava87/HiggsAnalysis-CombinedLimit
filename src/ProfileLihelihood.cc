#include "HiggsAnalysis/CombinedLimit/interface/ProfileLikelihood.h"
#include "RooRealVar.h"
#include "RooArgSet.h"
#include "RooRandom.h"
#include "RooDataSet.h"
#include "RooFitResult.h"
#include "RooStats/ProfileLikelihoodCalculator.h"
#include "RooStats/LikelihoodInterval.h"
#include "RooStats/HypoTestResult.h"
#include "HiggsAnalysis/CombinedLimit/interface/Combine.h"
#include "HiggsAnalysis/CombinedLimit/interface/CloseCoutSentry.h"


#include <Math/MinimizerOptions.h>

using namespace RooStats;

ProfileLikelihood::ProfileLikelihood() :
    LimitAlgo("Profile Likelihood specific options")
{
    options_.add_options()
        ("minimizerAlgo",      boost::program_options::value<std::string>(&minimizerAlgo_)->default_value("Minuit2"), "Choice of minimizer (Minuit vs Minuit2)")
        ("minimizerTolerance", boost::program_options::value<float>(&minimizerTolerance_)->default_value(1e-3),  "Tolerance for minimizer")
        ("maxTries",           boost::program_options::value<int>()->default_value(1), "Stop trying after N attempts per point")
        ("maxRelDeviation",    boost::program_options::value<float>()->default_value(0.05), "Max absolute deviation of the results from the median")
        ("maxOutlierFraction", boost::program_options::value<float>()->default_value(0.25), "Ignore up to this fraction of results if they're too far from the median")
        ("maxOutliers",        boost::program_options::value<int>()->default_value(3),      "Stop trying after finding N outliers")
        ("preFit", "Attept a fit before running the ProfileLikelihood calculator")
    ;
}

void ProfileLikelihood::applyOptions(const boost::program_options::variables_map &vm) 
{
    tries_    = vm.count("tries")    ? vm["tries"].as<unsigned int>() : 1;
    maxTries_ = vm["maxTries"].as<int>();
    maxRelDeviation_ = vm["maxRelDeviation"].as<float>();
    maxOutlierFraction_ = vm["maxOutlierFraction"].as<float>();
    maxOutliers_        = vm["maxOutliers"].as<int>();
    preFit_ = vm.count("preFit");
}

ProfileLikelihood::MinimizerSentry::MinimizerSentry(std::string &minimizerAlgo, double tolerance) :
    minimizerTypeBackup(ROOT::Math::MinimizerOptions::DefaultMinimizerType()),
    minimizerAlgoBackup(ROOT::Math::MinimizerOptions::DefaultMinimizerAlgo()),
    minimizerTollBackup(ROOT::Math::MinimizerOptions::DefaultTolerance())
{
  ROOT::Math::MinimizerOptions::SetDefaultTolerance(tolerance);
  if (minimizerAlgo.find(",") != std::string::npos) {
      size_t idx = minimizerAlgo.find(",");
      std::string type = minimizerAlgo.substr(0,idx), algo = minimizerAlgo.substr(idx+1);
      if (verbose > 1) std::cout << "Set default minimizer to " << type << ", algorithm " << algo << std::endl;
      ROOT::Math::MinimizerOptions::SetDefaultMinimizer(type.c_str(), algo.c_str());
  } else {
      if (verbose > 1) std::cout << "Set default minimizer to " << minimizerAlgo << std::endl;
      ROOT::Math::MinimizerOptions::SetDefaultMinimizer(minimizerAlgo.c_str());
  }
}

ProfileLikelihood::MinimizerSentry::~MinimizerSentry() 
{
  ROOT::Math::MinimizerOptions::SetDefaultTolerance(minimizerTollBackup);
  ROOT::Math::MinimizerOptions::SetDefaultMinimizer(minimizerTypeBackup.c_str(),minimizerAlgoBackup.empty() ? 0 : minimizerAlgoBackup.c_str());
}

bool ProfileLikelihood::run(RooWorkspace *w, RooAbsData &data, double &limit, const double *hint) { 
  MinimizerSentry minimizerConfig(minimizerAlgo_, minimizerTolerance_);
  CloseCoutSentry sentry(verbose < 0);

  bool success = false;
  std::vector<double> limits; double rMax = w->var("r")->getMax();  
  for (int i = 0; i < maxTries_; ++i) {
      w->loadSnapshot("clean");
      if (i > 0) { // randomize starting point
        w->var("r")->setMax(rMax*(0.5+RooRandom::uniform()));
        w->var("r")->setVal((0.1+0.5*RooRandom::uniform())*w->var("r")->getMax()); 
        if (withSystematics) { 
            RooArgSet set(*w->set("nuisances")); 
            RooDataSet *randoms = w->pdf("nuisancePdf")->generate(set, 1); 
            set = *randoms->get(0);
            if (verbose > 2) {
                std::cout << "Starting minimization from point " << std::endl;
                w->var("r")->Print("V");
                set.Print("V");
            }
            delete randoms;
        }
      }
      if (preFit_) {
        CloseCoutSentry sentry(verbose < 2);
        RooFitResult *res = w->pdf("model_s")->fitTo(data, RooFit::Save(1), RooFit::Minimizer("Minuit2"));
        if (res == 0 || res->covQual() != 3 || res->edm() > minimizerTolerance_) {
            if (verbose > 1) std::cout << "Fit failed (covQual " << (res ? res->covQual() : -1) << ", edm " << (res ? res->edm() : 0) << ")" << std::endl;
            continue;
        }
        if (verbose > 1) {
            res->Print("V");
            std::cout << "Covariance quality: " << res->covQual() << ", Edm = " << res->edm() << std::endl;
        }
        delete res;
      }
      bool thisTry = (doSignificance_ ?  runSignificance(w,data,limit) : runLimit(w,data,limit));
      if (!thisTry) continue;
      if (tries_ == 1) { success = true; break; }
      limits.push_back(limit); 
      int nresults = limits.size();
      if (nresults < tries_) continue;
      std::sort(limits.begin(), limits.end());
      double median = (nresults % 2 ? limits[nresults/2] : 0.5*(limits[nresults/2] + limits[nresults/2+1]));
      int noutlier = 0; double spreadIn = 0, spreadOut = 0;
      for (int j = 0; j < nresults; ++j) {
        double diff = fabs(limits[j]-median)/median;
        if (diff < maxRelDeviation_) { 
          spreadIn = max(spreadIn, diff); 
        } else {
          noutlier++;
          spreadOut = max(spreadOut, diff); 
        }
      }
      if (verbose > 0) {
          std::cout << "Numer of tries: " << i << "   Number of successes: " << nresults 
                    << ", Outliers: " << noutlier << " (frac = " << noutlier/double(nresults) << ")"
                    << ", Spread of non-outliers: " << spreadIn <<" / of outliers: " << spreadOut << std::endl;
      }
      if (noutlier <= maxOutlierFraction_*nresults) {
        if (verbose > 0) std::cout << " \\--> success! " << std::endl;
        success = true;
        break;
      } else if (noutlier > maxOutliers_) {
        if (verbose > 0) std::cout << " \\--> failure! " << std::endl;
        break;
      }
  }
  return success;
}

bool ProfileLikelihood::runLimit(RooWorkspace *w, RooAbsData &data, double &limit) {
  RooRealVar *r = w->var("r");
  RooArgSet  poi(*r);
  double rMax = r->getMax();
  bool success = false;
  CloseCoutSentry coutSentry(verbose <= 1); // close standard output and error, so that we don't flood them with minuit messages

  while (!success) {
    ProfileLikelihoodCalculator plcB(data, *w->pdf("model_s"), poi);
    plcB.SetConfidenceLevel(cl);
    std::auto_ptr<LikelihoodInterval> plInterval(plcB.GetInterval());
    if (plInterval.get() == 0) break;
    limit = plInterval->UpperLimit(*r); 
    if (limit >= 0.75*r->getMax()) { 
      std::cout << "Limit r < " << limit << "; r max < " << r->getMax() << std::endl;
      if (r->getMax()/rMax > 20) break;
      r->setMax(r->getMax()*2); 
      continue;
    }
    if (limit == r->getMin()) {
      std::cerr << "ProfileLikelihoodCalculator failed (returned upper limit equal to the lower bound)" << std::endl;
      break;
    }
    success = true;
  }
  coutSentry.clear();
  if (verbose >= 0) {
      if (success) {
        std::cout << "\n -- Profile Likelihood -- " << "\n";
        std::cout << "Limit: r < " << limit << " @ " << cl * 100 << "% CL" << std::endl;
      }
  }
  return success;
}

bool ProfileLikelihood::runSignificance(RooWorkspace *w, RooAbsData &data, double &limit) {
  RooRealVar *r = w->var("r");
  RooArgSet  poi(*r);

  ProfileLikelihoodCalculator plcS(data, *w->pdf("model_s"), poi);

  RooArgSet nullParamValues; 
  nullParamValues.addClone(*r); ((RooRealVar&)nullParamValues["r"]).setVal(0);
  plcS.SetNullParameters(nullParamValues);

  CloseCoutSentry coutSentry(verbose <= 1); // close standard output and error, so that we don't flood them with minuit messages
  std::auto_ptr<HypoTestResult> result(plcS.GetHypoTest());
  if (result.get() == 0) return false;
  coutSentry.clear();

  limit = result->Significance();
  if (limit == 0 && signbit(limit)) {
      std::cerr << "ProfileLikelihoodCalculator failed (returned significance -0)" << std::endl;
      return false;
  }
  std::cout << "\n -- Profile Likelihood -- " << "\n";
  std::cout << "Significance: " << limit << std::endl;
  return true;
}

