/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <algorithm>

#include <embree3/rtcore.h>
#include <embree3/rtcore_builder.h>

#include "luxrays/core/bvh/bvhbuild.h"
#include "luxrays/utils/atomic.h"
#include "slg/core/indexbvh.h"

// Required for explicit instantiations
#include "slg/engines/caches/photongi/photongicache.h"
// Required for explicit instantiations
#include "slg/lights/strategies/dlscacheimpl/dlscacheimpl.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Embree BVH node tree classes
//------------------------------------------------------------------------------

template<u_int CHILDREN_COUNT> class EmbreeBVHNode {
public:
	EmbreeBVHNode() { }
	virtual ~EmbreeBVHNode() { }
};

template<u_int CHILDREN_COUNT> class EmbreeBVHInnerNode : public EmbreeBVHNode<CHILDREN_COUNT> {
public:
	EmbreeBVHInnerNode() {
		for (u_int i = 0; i < CHILDREN_COUNT; ++i)
			children[i] = NULL;
	}
	virtual ~EmbreeBVHInnerNode() { }
	
	BBox bbox[CHILDREN_COUNT];
	EmbreeBVHNode<CHILDREN_COUNT> *children[CHILDREN_COUNT];
};

template<u_int CHILDREN_COUNT> class EmbreeBVHLeafNode : public EmbreeBVHNode<CHILDREN_COUNT> {
public:
	EmbreeBVHLeafNode(const size_t i) : index(i) { }
	virtual ~EmbreeBVHLeafNode() { }

	size_t index;
};

//------------------------------------------------------------------------------
// Embree builder data
//------------------------------------------------------------------------------

class IndexEmbreeBuilderGlobalData {
public:
	IndexEmbreeBuilderGlobalData();
	~IndexEmbreeBuilderGlobalData();

	RTCDevice embreeDevice;
	RTCBVH embreeBVH;

	u_int nodeCounter;
};

// IndexEmbreeBuilderGlobalData
IndexEmbreeBuilderGlobalData::IndexEmbreeBuilderGlobalData() {
	embreeDevice = rtcNewDevice(NULL);
	embreeBVH = rtcNewBVH(embreeDevice);

	nodeCounter = 0;
}

IndexEmbreeBuilderGlobalData::~IndexEmbreeBuilderGlobalData() {
	rtcReleaseBVH(embreeBVH);
	rtcReleaseDevice(embreeDevice);
}
//------------------------------------------------------------------------------
// BuildEmbreeBVHArray
//------------------------------------------------------------------------------

static inline void CopyBBox(const float *src, float *dst) {
	*dst++ = *src++;
	*dst++ = *src++;
	*dst++ = *src++;

	*dst++ = *src++;
	*dst++ = *src++;
	*dst = *src;
}

template<u_int CHILDREN_COUNT, class T> static u_int BuildEmbreeBVHArray(
		const EmbreeBVHNode<CHILDREN_COUNT> *node, const vector<T> &allEntries,
		u_int offset, slg::ocl::IndexBVHArrayNode *bvhArrayTree) {
	if (node) {
		slg::ocl::IndexBVHArrayNode *arrayNode = &bvhArrayTree[offset];

		const EmbreeBVHInnerNode<CHILDREN_COUNT> *innerNode = dynamic_cast<const EmbreeBVHInnerNode<CHILDREN_COUNT> *>(node);

		if (innerNode) {
			// It is an inner node

			++offset;

			BBox bbox;
			for (u_int i = 0; i < CHILDREN_COUNT; ++i) {
				if (innerNode->children[i]) {
					// Add the child tree to the array
					const u_int childIndex = offset;
					offset = BuildEmbreeBVHArray<CHILDREN_COUNT, T>(innerNode->children[i], allEntries, childIndex, bvhArrayTree);
					if (dynamic_cast<const EmbreeBVHInnerNode<CHILDREN_COUNT> *>(innerNode->children[i])) {
						// If the child was an inner node, set the skip index
						bvhArrayTree[childIndex].nodeData = offset;
					}
					
					bbox = Union(bbox, innerNode->bbox[i]);
				}
			}

			CopyBBox(&bbox.pMin.x, &arrayNode->bvhNode.bboxMin[0]);
		} else {
			// Must be a leaf
			const EmbreeBVHLeafNode<CHILDREN_COUNT> *leaf = (const EmbreeBVHLeafNode<CHILDREN_COUNT> *)node;
			arrayNode->entryLeaf.entryIndex = leaf->index;

			++offset;
			// Mark as a leaf
			arrayNode->nodeData = offset | 0x80000000u;
		}
	}

	return offset;
}

//------------------------------------------------------------------------------
// BuildEmbreeBVH
//------------------------------------------------------------------------------

template<u_int CHILDREN_COUNT> static void *CreateNodeFunc(RTCThreadLocalAllocator allocator,
		unsigned int numChildren, void *userPtr) {
	assert (numChildren <= CHILDREN_COUNT);

	IndexEmbreeBuilderGlobalData *gd = (IndexEmbreeBuilderGlobalData *)userPtr;
	AtomicInc(&gd->nodeCounter);

	return new (rtcThreadLocalAlloc(allocator, sizeof(EmbreeBVHInnerNode<CHILDREN_COUNT>), 16)) EmbreeBVHInnerNode<CHILDREN_COUNT>();
}

template<u_int CHILDREN_COUNT> static void *CreateLeafFunc(RTCThreadLocalAllocator allocator,
		const RTCBuildPrimitive *prims, size_t numPrims, void *userPtr) {
	// RTCBuildSettings::maxLeafSize is set to 1 
	assert (numPrims == 1);

	IndexEmbreeBuilderGlobalData *gd = (IndexEmbreeBuilderGlobalData *)userPtr;
	AtomicInc(&gd->nodeCounter);

	return new (rtcThreadLocalAlloc(allocator, sizeof(EmbreeBVHLeafNode<CHILDREN_COUNT>), 16)) EmbreeBVHLeafNode<CHILDREN_COUNT>(prims[0].primID);
}

template<u_int CHILDREN_COUNT> static void NodeSetChildrensPtrFunc(void *nodePtr, void **children, unsigned int numChildren, void *userPtr) {
	assert (numChildren <= CHILDREN_COUNT);

	EmbreeBVHInnerNode<CHILDREN_COUNT> *node = (EmbreeBVHInnerNode<CHILDREN_COUNT> *)nodePtr;

	for (u_int i = 0; i < numChildren; ++i)
		node->children[i] = (EmbreeBVHNode<CHILDREN_COUNT> *)children[i];
}

template<u_int CHILDREN_COUNT> static void NodeSetChildrensBBoxFunc(void *nodePtr,
		const RTCBounds **bounds, unsigned int numChildren, void *userPtr) {
	EmbreeBVHInnerNode<CHILDREN_COUNT> *node = (EmbreeBVHInnerNode<CHILDREN_COUNT> *)nodePtr;

	for (u_int i = 0; i < numChildren; ++i) {
		node->bbox[i].pMin.x = bounds[i]->lower_x;
		node->bbox[i].pMin.y = bounds[i]->lower_y;
		node->bbox[i].pMin.z = bounds[i]->lower_z;

		node->bbox[i].pMax.x = bounds[i]->upper_x;
		node->bbox[i].pMax.y = bounds[i]->upper_y;
		node->bbox[i].pMax.z = bounds[i]->upper_z;
	}
}

//------------------------------------------------------------------------------
// BuildEmbreeBVH
//------------------------------------------------------------------------------

template<u_int CHILDREN_COUNT, class T> static slg::ocl::IndexBVHArrayNode *BuildEmbreeBVH(
		RTCBuildQuality quality, const vector<T> &allEntries,
		const float entryRadius, u_int *nNodes) {
	//const double t1 = WallClockTime();

	// Initialize RTCPrimRef vector
	vector<RTCBuildPrimitive> prims(allEntries.size());
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int i = 0; i < prims.size(); ++i) {
		RTCBuildPrimitive &prim = prims[i];
		const T &entry = allEntries[i];

		prim.lower_x = entry.p.x - entryRadius;
		prim.lower_y = entry.p.y - entryRadius;
		prim.lower_z = entry.p.z - entryRadius;
		prim.geomID = 0;

		prim.upper_x = entry.p.x + entryRadius;
		prim.upper_y = entry.p.y + entryRadius;
		prim.upper_z = entry.p.z + entryRadius;
		prim.primID = i;
	}

	//const double t2 = WallClockTime();
	//cout << "BuildEmbreeBVH preprocessing time: " << int((t2 - t1) * 1000) << "ms\n";

	RTCBuildArguments buildArgs = rtcDefaultBuildArguments();
	buildArgs.buildQuality = quality;
	buildArgs.maxBranchingFactor = CHILDREN_COUNT;
	buildArgs.maxLeafSize = 1;
	
	IndexEmbreeBuilderGlobalData *globalData = new IndexEmbreeBuilderGlobalData();
	buildArgs.bvh = globalData->embreeBVH;
	buildArgs.primitives = &prims[0];
	buildArgs.primitiveCount = prims.size();
	buildArgs.primitiveArrayCapacity = prims.size();
	buildArgs.createNode = &CreateNodeFunc<CHILDREN_COUNT>;
	buildArgs.setNodeChildren = &NodeSetChildrensPtrFunc<CHILDREN_COUNT>;
	buildArgs.setNodeBounds = &NodeSetChildrensBBoxFunc<CHILDREN_COUNT>;
	buildArgs.createLeaf = &CreateLeafFunc<CHILDREN_COUNT>;
	buildArgs.splitPrimitive = NULL;
	buildArgs.buildProgress = NULL;
	buildArgs.userPtr = globalData;

	EmbreeBVHNode<CHILDREN_COUNT> *root = (EmbreeBVHNode<CHILDREN_COUNT> *)rtcBuildBVH(&buildArgs);

	*nNodes = globalData->nodeCounter;

	//const double t3 = WallClockTime();
	//cout << "BuildEmbreeBVH rtcBVHBuilderBinnedSAH time: " << int((t3 - t2) * 1000) << "ms\n";

	slg::ocl::IndexBVHArrayNode *bvhArrayTree = new slg::ocl::IndexBVHArrayNode[*nNodes];
	bvhArrayTree[0].nodeData = BuildEmbreeBVHArray<CHILDREN_COUNT, T>(root, allEntries, 0, bvhArrayTree);
	// If root was a leaf, mark the node
	if (dynamic_cast<const EmbreeBVHLeafNode<CHILDREN_COUNT> *>(root))
		bvhArrayTree[0].nodeData |= 0x80000000u;

	//const double t4 = WallClockTime();
	//cout << "BuildEmbreeBVH BuildEmbreeBVHArray time: " << int((t4 - t3) * 1000) << "ms\n";

	delete globalData;

	return bvhArrayTree;
}

//------------------------------------------------------------------------------
// IndexBvh
//------------------------------------------------------------------------------

template <class T>
IndexBvh<T>::IndexBvh(const vector<T> &entries, const float radius) :
		allEntries(entries), entryRadius(radius), entryRadius2(radius * radius) {
	arrayNodes = BuildEmbreeBVH<4, T>(RTC_BUILD_QUALITY_HIGH, allEntries, entryRadius, &nNodes);
}

template <class T>
IndexBvh<T>::~IndexBvh() {
	delete arrayNodes;
}

//------------------------------------------------------------------------------
// Explicit instantiations
//------------------------------------------------------------------------------

// C++ can be quite horrible...

namespace slg {
template class IndexBvh<Photon>;
template class IndexBvh<RadiancePhoton>;
template class IndexBvh<DLSCacheEntry>;
}
