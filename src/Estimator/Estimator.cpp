// Estimator.cpp - Common code for estimators.
// See Estimator.hpp for details.
//
// Copyright (c) 2017 Timothy Zhu.
// Licensed under the MIT License. See LICENSE file for details.
//

#include <stdexcept>
#include <json/json.h>
#include "Estimator.hpp"

using namespace std;

Estimator* Estimator::create(const Json::Value& estimatorInfo)
{
    string type = estimatorInfo["type"].asString();
    if (type == "networkIn") {
        return new NetworkInEstimator(estimatorInfo);
    } else if (type == "networkOut") {
        return new NetworkOutEstimator(estimatorInfo);
    } else if (type == "storageSSD") {
        return new StorageSSDEstimator(estimatorInfo);
    } else {
        throw invalid_argument("Invalid estimator type " + type);
    }
}
