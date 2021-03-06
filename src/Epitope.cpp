#include "Epitope.h"
#include "BCell.h"
#include "PlasmaCell.h"
#include <random>
#include <vector>
#include <iostream>
#include <cassert>
#include "zppsim_random.hpp"
#include "Antigen.h"
#include "shared.h"

using namespace std;
using namespace zppdb;

Epitope::Epitope(
	SimParameters & params, Antigen * antigenPtr,
	uint32_t neighborSeed,
	uint32_t energySeed,
	zppsim::rng_t & rng,
	int64_t row, int64_t col, double energyMean,
	zppdb::Database * dbPtr,
	zppdb::Table<AntigenNeighborRow> * neighborTablePtr, zppdb::Table<AntigenEnergyRow> * energyTablePtr
) :
	antigenPtr(antigenPtr),
	energySeedBytes(hostToNetworkBytes(energySeed)),
	nLoci(antigenPtr->nLoci),
	nInteractions(params.epitopes.nInteractions),
	alphabetSize(params.sequences.alphabetSize),
	alphabet(params.sequences.alphabet),
	pEnergyMutation(params.epitopes.pEnergyMutation[row][col]),
	pNeighborMutation(params.epitopes.pNeighborMutation[row][col]),
	row(row), col(col),
	energyMean(energyMean),
	energyDist(0.0, 1.0),
	dbPtr(dbPtr),
	neighborTablePtr(neighborTablePtr),
	energyTablePtr(energyTablePtr)
{
	assert(energySeed > 0);
	assert(nLoci <= std::numeric_limits<uint16_t>::max());
	
	zppsim::rng_t rngNeighbors(neighborSeed);
	
	neighbors = vector<vector<uint32_t>>(nLoci, vector<uint32_t>(nInteractions));
	activeNeighborSeqs = vector<vector<Sequence>>(nLoci);
//	energyMaps = vector<unordered_map<Sequence, double, HashSequence>>(nLoci);
	
	for(uint32_t i = 0; i < nLoci; i++) {
		// Uniformly chosen neighbors
		vector<uint32_t> locusNeighbors = zppsim::drawUniformIndicesExcept(
			rngNeighbors, uint32_t(nLoci), uint32_t(nInteractions), i
		);
		for(uint32_t j = 0; j < nInteractions; j++) {
			neighbors[i][j] = locusNeighbors[j];
		}
		
		// Energies generated as needed (deterministically from energySeed)
	}
	
	writeNeighborsToDatabase();
}

void Epitope::mutate(zppsim::rng_t & rng) {
	// TODO: define mutation under new energy scheme
	assert(false);
	
	/*energyCache.clear();
	
	// Randomly mutate energies (among those that have been assigned)
	// and neighbors assignments
	for(uint32_t i = 0; i < nLoci; i++) {
		// Make sure we have < 2^32 - 1 neighbor sequences.
		// (If we hit this limit, probably time to re-evaluate the code.)
		assert(activeNeighborSeqs[i].size() < std::numeric_limits<uint32_t>::max());
		
		// Randomly draw sequences and mutate energies
		auto seqIndices = zppsim::drawMultipleBernoulli(rng, uint32_t(activeNeighborSeqs[i].size()), pEnergyMutation);
		for(uint32_t seqIndex : seqIndices) {
			Sequence seq = activeNeighborSeqs[i][seqIndex];
			double energy = energyDist(rng);
			energyMaps[i][seq] = energy;
			assert(activeNeighborSeqs[i].size() == energyMaps[i].size());
			writeEnergyToDatabase(i, seq, energy);
		}
		
		// Randomly draw neighbor indexes and modify them
		bernoulli_distribution shouldMutateNeighbor(pNeighborMutation);
		vector<uint32_t> neighborIndexesToChange;
		vector<uint32_t> disallowedNeighbors;
		disallowedNeighbors.push_back(i);
		for(uint32_t j = 0; j < nInteractions; j++) {
			if(shouldMutateNeighbor(rng)) {
				neighborIndexesToChange.push_back(j);
			}
			else {
				disallowedNeighbors.push_back(neighbors[i][j]);
			}
		}
		
		vector<uint32_t> newNeighbors = zppsim::drawUniformIndicesExcept(
			rng, uint32_t(nLoci), uint32_t(neighborIndexesToChange.size()), disallowedNeighbors
		);
		assert(neighborIndexesToChange.size() == newNeighbors.size());
		for(uint32_t j = 0; j < neighborIndexesToChange.size(); j++) {
			neighbors[i][neighborIndexesToChange[j]] = newNeighbors[j];
			writeNeighborToDatabase(
				i, neighborIndexesToChange[j], newNeighbors[j]
			);
		}
	}*/
}

void Epitope::writeNeighborsToDatabase()
{
	if(!dbPtr->tableExists(*neighborTablePtr)) {
		return;
	}
	
	AntigenNeighborRow dbRow;
	dbRow.infection_id = antigenPtr->id;
	dbRow.epitope_row = row;
	dbRow.epitope_column = col;
	for(uint32_t l = 0; l < nLoci; l++) {
		dbRow.locus = int64_t(l);
		for(uint8_t n = 0; n < nInteractions; n++) {
			dbRow.neighbor_index = n;
			dbRow.neighbor_locus = neighbors[l][n];
			dbPtr->insert(*neighborTablePtr, dbRow);
		}
	}
}

void Epitope::writeNeighborToDatabase(
	uint32_t locus, uint32_t neighborIndex, uint32_t neighborLocus
) {
	if(!dbPtr->tableExists(*neighborTablePtr)) {
		return;
	}
	
	AntigenNeighborRow dbRow;
	dbRow.infection_id = antigenPtr->id;
	dbRow.epitope_row = row;
	dbRow.epitope_column = col;
	dbRow.locus = locus;
	dbRow.neighbor_index = neighborIndex;
	dbRow.neighbor_locus = neighborLocus;
	dbPtr->insert(*neighborTablePtr, dbRow);
}

void Epitope::writeEnergyToDatabase(
	uint32_t locus, Sequence const & seq, double energy
) {
	if(!dbPtr->tableExists(*energyTablePtr)) {
		return;
	}
	
	AntigenEnergyRow dbRow;
	dbRow.infection_id = antigenPtr->id;
	dbRow.epitope_row = row;
	dbRow.epitope_column = col;
	dbRow.locus = locus;
	dbRow.neighbor_sequence = seq.toString(alphabet);
	dbRow.energy = energy;
	dbPtr->insert(*energyTablePtr, dbRow);
}

double Epitope::getEnergy(
	BCell const & cell
) {
	double energy = 0.0;
	
	uint32_t seqId = cell.getSequenceId();
	
	auto itr = energyCache.find(seqId);
	if(itr == energyCache.end()) {
		// Add up (epistatic) energy component from each locus
		for(uint32_t i = 0; i < nLoci; i++) {
			energy += getEnergy(cell, i);
		}
		energy /= sqrt(nLoci);
		energy += energyMean;
		energyCache[(seqId)] = energy;
	}
	else {
		energy = itr->second;
	}
	assert(energy > -std::numeric_limits<double>::infinity());
	
	return energy;
}

double Epitope::getEnergy(
	BCell const & cell, uint16_t locus
) {
	// Construct "neighbor sequence" key of bytes:
	// - energy seed (4 bytes, network byte order)
	// - locus (2 bytes, network byte order)
	// - amino acid at locus and each neighbor (1 byte each)
	vector<uint8_t> keyBytes;
	keyBytes.reserve(4 + 2 + 1 + nInteractions);
	keyBytes.insert(keyBytes.end(), energySeedBytes.begin(), energySeedBytes.end());
	keyBytes.push_back((locus & 0xFF00) >> 8);
	keyBytes.push_back(locus & 0x00FF);
	keyBytes.push_back(cell.get(locus));
	for(uint32_t i = 0; i < nInteractions; i++) {
		keyBytes.push_back(cell.get(neighbors[locus][i]));
	}
	
	return sha1Normal(keyBytes);
	
	// Get existing energy for key if present; generate if not present
//	double energy;
//	auto itr = energyMaps[locus].find(seq);
//	if(itr == energyMaps[locus].end()) {
//		energy = energyDist(rng);
//		energyMaps[locus][seq] = energy;
//		activeNeighborSeqs[locus].push_back(seq);
//		assert(energyMaps[locus].size() == activeNeighborSeqs[locus].size());
//		writeEnergyToDatabase(locus, seq, energy);
//	}
//	else {
//		energy = itr->second;
//	}
//	
//	assert(!std::isinf(energy));
//	
//	return energy;
}


void Epitope::verifyNeighbors()
{
//	for(uint32_t i = 0; i < N_LOCI; i++) {
//		for(uint32_t j = 0; j < N_INTERACTIONS; j++) {
//			uint32_t neighbor = locusInfo[i].neighbors[j];
//			if(neighbor == i) {
//				throw std::runtime_error(strprintf("locus %u has self as neighbor %u", i, j));
//			}
//			for(uint32_t k = 0; k < N_INTERACTIONS; k++) {
//				if(j != k && neighbor == locusInfo[i].neighbors[k]) {
//					throw std::runtime_error(strprintf("locus %u has %u as neighbors %u and %u", i, neighbor, j, k));
//				}
//			}
//		}
//	}
}

void Epitope::verifyEnergy()
{
//	for(uint32_t i = 0; i < N_LOCI; i++) {
//		auto energyFunc = [this,&i](uint32_t index) -> double {
//			return locusInfo[i].energy[index];
//		};
//		
//		double mean = meanFromFunction(energyFunc, nPossibleSeqs);
//		double sd = standardDeviationSample(mean, energyFunc, nPossibleSeqs);
//		double se = sd / sqrt(nPossibleSeqs);
//		
//		// Mean within 4 standard errors
//		if(mean < energyMean - 4.5*se || mean > energyMean + 4.5*se) {
//			throw std::runtime_error(strprintf("mean energy for locus %u is %lf\n", i, mean));
//		}
//		
//		// Estimate of sd within hopefully-broad-enough range
//		// TODO: get proper bounds using chi-squared sampling distribution
//		if(sd < 0.7 || sd > 1.3) {
//			throw std::runtime_error(strprintf("sd energy for locus %u is %lf\n", i, sd));
//		}
//	}
}
