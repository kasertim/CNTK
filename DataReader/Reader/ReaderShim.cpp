//
// <copyright file="ReaderShim.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// HTKMLFReaderShim.cpp: implementation for shim that wraps new HTKMLF reader
//

#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN32
#include <objbase.h>
#endif
#include "Basics.h"

#define DATAREADER_EXPORTS  // creating the exports here
#include "DataReader.h"
#include "commandArgUtil.h"
#include "ReaderShim.h"

#include "SubstitutingMemoryProvider.h"
#include "HeapMemoryProvider.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    template<class ElemType>
    ReaderShim<ElemType>::ReaderShim(ReaderFactory factory)
        : m_layout(make_shared<MBLayout>())
        , m_factory(factory)
    {
    }

    template<class ElemType>
    void ReaderShim<ElemType>::Init(const ConfigParameters& config)
    {
        intargvector numberOfuttsPerMinibatchForAllEpochs =
            config(L"nbruttsineachrecurrentiter", ConfigParameters::Array(intargvector(vector<int>{ 1 })));

        auto numSeqsPerMBForAllEpochs = numberOfuttsPerMinibatchForAllEpochs;
        m_layout->Init(numSeqsPerMBForAllEpochs[0], 0, true);

        m_reader = m_factory(config);
        m_inputs = m_reader->GetInputs();
        for (auto i : m_inputs)
        {
            m_nameToInputId.insert(std::make_pair(i->name, i->id));
        }
    }

    template<class ElemType>
    void ReaderShim<ElemType>::StartMinibatchLoop(size_t mbSize, size_t epoch, size_t requestedEpochSamples = requestDataSize)
    {
        return StartDistributedMinibatchLoop(mbSize, epoch, 0, 1, requestedEpochSamples);
    }

    template<class ElemType>
    void ReaderShim<ElemType>::StartDistributedMinibatchLoop(
        size_t requestedMBSize,
        size_t epoch,
        size_t subsetNum,
        size_t numSubsets,
        size_t requestedEpochSamples /*= requestDataSize*/)
    {
        EpochConfiguration config;
        config.workerRank = subsetNum;
        config.numberOfWorkers = numSubsets;
        config.minibatchSize = requestedMBSize;
        config.totalSize = requestedEpochSamples;
        config.index = epoch;

        m_epoch = m_reader->StartNextEpoch(config);
    }

    template<class ElemType>
    bool ReaderShim<ElemType>::GetMinibatch(std::map<std::wstring, Matrix<ElemType>*>& matrices)
    {
        // Check that all matrices have the same device id.
        // If not we should inject the IMemoryProvider per input.
        int deviceId = matrices.begin()->second->GetDeviceId();
        for (auto mx : matrices)
        {
            if (mx.second->GetDeviceId() != deviceId)
            {
                assert(false);
            }
        }

        Minibatch m = m_epoch->ReadMinibatch();
        if (!m.atEndOfEpoch)
        {
            // Copy returned minibatch to the matrices.
            for (const auto& mx : matrices)
            {
                assert(m_nameToInputId.find(mx.first) != m_nameToInputId.end());

                const auto& input = m.minibatch[m_nameToInputId[mx.first]];
                LayoutPtr layout = input->GetLayout();
                m_layout = layout->columns;

                size_t columnNumber = layout->columns->GetNumCols();
                size_t rowNumber = layout->rows->GetNumElements();

                auto data = reinterpret_cast<const ElemType*>(input->GetData());
                mx.second->SetValue(rowNumber, columnNumber, mx.second->GetDeviceId(), const_cast<ElemType*>(data), matrixFlagNormal);
            }
        }

        return m.atEndOfEpoch;
    }

    template<class ElemType>
    bool ReaderShim<ElemType>::DataEnd(EndDataType /*endDataType*/)
    {
        return false;
    }

    template<class ElemType>
    void ReaderShim<ElemType>::CopyMBLayoutTo(MBLayoutPtr layout)
    {
        layout->CopyFrom(m_layout);
    }

    template<class ElemType>
    size_t ReaderShim<ElemType>::GetNumParallelSequences()
    {
        return m_layout->GetNumParallelSequences();
    }

    template class ReaderShim<float>;
    template class ReaderShim<double>;

}}}