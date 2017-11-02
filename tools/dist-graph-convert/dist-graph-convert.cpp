/** Distributed graph converter -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2017, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * Distributed graph converter tool based on shared-memory graph convert.
 *
 * @author Loc Hoang <l_hoang@utexas.edu>
 */

#include <utility>

#include "galois/DistGalois.h"
#include "llvm/Support/CommandLine.h"

#include "dist-graph-convert-helpers.h"

namespace cll = llvm::cl;

// TODO: move these enums to a common location for all graph convert tools
enum ConvertMode {
  edgelist2gr,
  gr2wgr,
  gr2tgr,
  edgelistb2gr // TODO
};

enum EdgeType {
  uint32_,
  void_
};

static cll::opt<std::string> inputFilename(cll::Positional, 
    cll::desc("<input file>"), cll::Required);
static cll::opt<std::string> outputFilename(cll::Positional,
    cll::desc("<output file>"), cll::init(std::string()));
static cll::opt<EdgeType> edgeType("edgeType", 
    cll::desc("Input/Output edge type:"),
    cll::values(
      clEnumValN(EdgeType::uint32_, "uint32", 
                 "32 bit unsigned int edge values"),
      clEnumValN(EdgeType::void_, "void", 
                 "no edge values"),
      clEnumValEnd), 
    cll::init(EdgeType::void_));
static cll::opt<ConvertMode> convertMode(cll::desc("Conversion mode:"),
    cll::values(
      clEnumVal(edgelistb2gr, "Convert edge list binary to binary gr"),
      clEnumVal(edgelist2gr, "Convert edge list to binary gr"),
      clEnumVal(gr2wgr, "Convert unweighted binary gr to weighted binary gr "
                         "(in-place)"),
      clEnumVal(gr2tgr, "Convert (weighted) binary gr to transpose binary gr "),
      clEnumValEnd
    ), cll::Required);
static cll::opt<unsigned> totalNumNodes("numNodes", 
                                        cll::desc("Nodes in input graph"),
                                        cll::init(0));
static cll::opt<unsigned> threadsToUse("t", cll::desc("Threads to use"),
                                       cll::init(1));
static cll::opt<bool> editInPlace("inPlace", 
                                  cll::desc("Flag specifying conversion is in "
                                            "place"),
                                  cll::init(false));


// Base structures to inherit from: name specifies what the converter can do
struct Conversion { };
struct HasOnlyVoidSpecialization { };
struct HasNoVoidSpecialization { };

////////////////////////////////////////////////////////////////////////////////
// BEGIN CONVERT CODE/STRUCTS
////////////////////////////////////////////////////////////////////////////////

/**
 * Convert 1: figure out edge type, then call convert with edge type as
 * an additional template argument.
 */
template<typename C>
void convert() {
  C c;

  switch (edgeType) {
    case EdgeType::uint32_: convert<uint32_t>(c, c); break;
    case EdgeType::void_: convert<void>(c, c); break;
    default: abort();
  };
}

/**
 * Convert 2 called from convert above: calls convert from the appropriate
 * structure
 */
template<typename EdgeTy, typename C>
void convert(C& c, Conversion) {
  auto& net = galois::runtime::getSystemNetworkInterface();

  if (net.ID == 0) {
    printf("Input: %s; Output: %s\n", inputFilename.c_str(), 
                                      outputFilename.c_str());
  }

  galois::runtime::getHostBarrier().wait();

  galois::StatTimer convertTimer("Convert Time", "convert"); 
  convertTimer.start();
  c.template convert<EdgeTy>(inputFilename, outputFilename);
  convertTimer.stop();

  if (net.ID == 0) {
    galois::gPrint("Done with convert\n");
  }
}

/**
 * Converts an edge list to a Galois binary graph.
 */
struct Edgelist2Gr : public Conversion {
  // WARNING: WILL NOT WORK IF THE EDGE LIST HAS WEIGHTS
  template<typename EdgeTy>
  void convert(const std::string& inputFile, const std::string& outputFile) {
    GALOIS_ASSERT((totalNumNodes != 0), "edgelist2gr needs num nodes");
    GALOIS_ASSERT(!(outputFile.empty()), "edgelist2gr needs an output file");

    if (!std::is_void<EdgeTy>::value) {
      GALOIS_DIE("Currently not implemented for non-void\n");
    }

    auto& net = galois::runtime::getSystemNetworkInterface();
    uint64_t hostID = net.ID;

    std::ifstream edgeListFile(inputFile.c_str());
    uint64_t fileSize = getFileSize(edgeListFile);
    if (hostID == 0) {
      printf("File size is %lu\n", fileSize);
    }

    uint64_t localStartByte;
    uint64_t localEndByte;
    std::tie(localStartByte, localEndByte) = determineByteRange(edgeListFile,
                                                                fileSize);
    //printf("[%lu] Byte start %lu byte end %lu, num bytes %lu\n", hostID, 
    //               localStartByte, localEndByte, localEndByte - localStartByte);

    // load edges into a vector
    std::vector<uint32_t> localEdges = loadEdgesFromEdgeList<EdgeTy>(
                                           edgeListFile, localStartByte,
                                           localEndByte, totalNumNodes);
    edgeListFile.close();

    uint64_t totalEdgeCount = accumulateValue(getNumEdges<EdgeTy>(localEdges));

    if (hostID == 0) {
      printf("Total num edges %lu\n", totalEdgeCount);
    }

    std::vector<std::pair<uint64_t, uint64_t>> hostToNodes = 
        getEvenNodeToHostMapping<EdgeTy>(localEdges, totalNumNodes, 
                                         totalEdgeCount);

    uint64_t localNodeBegin = hostToNodes[hostID].first;
    uint64_t localNodeEnd = hostToNodes[hostID].second;
    uint64_t localNumNodes = localNodeEnd - localNodeBegin;

    sendEdgeCounts<EdgeTy>(hostToNodes, localEdges);
    std::atomic<uint64_t> edgesToReceive;
    edgesToReceive.store(receiveEdgeCounts());

    printf("[%lu] Need to receive %lu edges\n", hostID, edgesToReceive.load());

    // FIXME ONLY V1 SUPPORT
    std::vector<std::vector<uint32_t>> localSrcToDest(localNumNodes);
    std::vector<std::vector<uint32_t>> localSrcToData;
    std::vector<std::mutex> nodeLocks(localNumNodes);

    sendAssignedEdges(hostToNodes, localEdges, localSrcToDest, nodeLocks);
    freeVector(localEdges);
    receiveAssignedEdges(edgesToReceive, hostToNodes, localSrcToDest, 
                         nodeLocks);
    freeVector(hostToNodes);
    freeVector(nodeLocks);

    // TODO can refactor to function
    uint64_t totalAssignedEdges = 0;
    for (unsigned i = 0; i < localNumNodes; i++) {
      totalAssignedEdges += localSrcToDest[i].size();
    }

    printf("[%lu] Will write %lu edges\n", hostID, totalAssignedEdges);

    // calculate global edge offset using edge counts from other hosts
    std::vector<uint64_t> edgesPerHost = getEdgesPerHost(totalAssignedEdges);
    uint64_t globalEdgeOffset = 0;
    uint64_t totalEdgeCount2 = 0;
    for (unsigned h = 0; h < hostID; h++) {
      globalEdgeOffset += edgesPerHost[h];
      totalEdgeCount2 += edgesPerHost[h];
    }

    uint64_t totalNumHosts = net.Num;
    // finish off getting total edge count (note this is more of a sanity check
    // since we got total edge count near the beginning already)
    for (unsigned h = hostID; h < totalNumHosts; h++) {
      totalEdgeCount2 += edgesPerHost[h];
    }
    GALOIS_ASSERT(totalEdgeCount == totalEdgeCount2);
    freeVector(edgesPerHost);

    writeToGr(outputFile, totalNumNodes, totalEdgeCount2, localNumNodes, 
              localNodeBegin, globalEdgeOffset, localSrcToDest, localSrcToData);

    galois::runtime::getHostBarrier().wait();
  }
};

/**
 * Transpose a WEIGHTED Galois binary graph.
 * 
 * TODO make it work for non weighted graphs too
 */
struct Gr2TGr : public Conversion {
  template<typename EdgeTy>
  void convert(const std::string& inputFile, const std::string& outputFile) {
    GALOIS_ASSERT(!(outputFile.empty()), "gr2tgr needs an output file");
    //auto& net = galois::runtime::getSystemNetworkInterface();
    //uint32_t hostID = net.ID;
    //uint32_t totalNumHosts = net.Num;

    //std::pair<uint64_t, uint64_t> nodesToRead;
    //std::pair<uint64_t, uint64_t> edgesToRead;

    // TODO
    // get "read" assignment of nodes
    //uint64_t nodesToRead = getNodesToReadFromGr(inputFile);
    //printf("[%u] Reads nodes %lu to %lu\n", hostID, nodesToRead.first,
    //                                                nodesToRead.second);
    // read edges of assigned nodes using MPI_Graph, load into the same format
    // used by edgelist2gr; catch is to do it TRANSPOSED
    //std::vector<uint32_t> localEdges = 
    //    loadTransposedEdgesFromMPIGraph(inputFile, nodesToRead);

    // TODO
    // at this point you can do the same thing as edgelist2gr and it will work
    // fine
  }
};


/**
 * Adds random weights to a Galois binary graph.
 */
struct Gr2WGr : public Conversion {
  template<typename EdgeTy>
  void convert(const std::string& inputFile, const std::string& outputFile) {
    GALOIS_ASSERT(outputFile.empty(), "gr2wgr doesn't take an output file");
    GALOIS_ASSERT(editInPlace, "You must use -inPlace with gr2wgr");

    MPI_File unweightedGr;
    MPICheck(MPI_File_open(MPI_COMM_WORLD, inputFile.c_str(), 
                           MPI_MODE_RDWR, MPI_INFO_NULL, &unweightedGr));
    uint64_t grHeader[4];
    // read gr header for metadata
    MPICheck(MPI_File_read_at(unweightedGr, 0, grHeader, 4, MPI_UINT64_T, 
                              MPI_STATUS_IGNORE));
    GALOIS_ASSERT(grHeader[0] == 1, "gr file must be version 1 for convert");

    uint64_t totalNumEdges = grHeader[3];
    uint64_t localEdgeBegin;
    uint64_t localEdgeEnd;
    std::tie(localEdgeBegin, localEdgeEnd) = getLocalAssignment(totalNumEdges);
    
    uint64_t hostID = galois::runtime::getSystemNetworkInterface().ID;
    printf("[%lu] Responsible for edges %lu to %lu\n", hostID, localEdgeBegin,
                                                       localEdgeEnd);
    
    uint64_t numLocalEdges = localEdgeEnd - localEdgeBegin;
    std::vector<uint32_t> edgeDataToWrite = generateRandomNumbers(numLocalEdges,
                                            hostID, 1, 100);
    GALOIS_ASSERT(edgeDataToWrite.size() == numLocalEdges);
    uint64_t byteOffsetToEdgeData = getOffsetToLocalEdgeData(grHeader[2], 
                                      totalNumEdges, localEdgeBegin);
    writeEdgeDataData(unweightedGr, numLocalEdges, byteOffsetToEdgeData, 
                      edgeDataToWrite);

    // if host 0 update header with edge size
    if (hostID == 0) {
      uint64_t edgeSize = 4;
      MPICheck(MPI_File_write_at(unweightedGr, sizeof(uint64_t), 
                                 &edgeSize, 1, MPI_UINT64_T, MPI_STATUS_IGNORE));
    }

    MPICheck(MPI_File_close(&unweightedGr));
  }
};

int main(int argc, char** argv) {
  galois::DistMemSys G;
  llvm::cl::ParseCommandLineOptions(argc, argv);
  galois::setActiveThreads(threadsToUse);

  switch (convertMode) {
    //case edgelistb2gr: 
    //  convert<EdgelistB2Gr>(); break;
    case edgelist2gr: 
      convert<Edgelist2Gr>(); break;
    case gr2wgr: 
      convert<Gr2WGr>(); break;
    case gr2tgr: 
      convert<Gr2TGr>(); break;
    default: abort();
  }
  return 0;
}
