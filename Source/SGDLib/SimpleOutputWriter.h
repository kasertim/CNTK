//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#pragma once

#include "Basics.h"
#include "DataReader.h"
#include "ComputationNetwork.h"
#include "DataReaderHelpers.h"
#include "Helpers.h"
#include "File.h"
#include "fileutil.h"
#include <vector>
#include <string>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#include "ProgressTracing.h"

using namespace std;

namespace Microsoft { namespace MSR { namespace CNTK {

template <class ElemType>
class SimpleOutputWriter
{
    typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;

private:
    std::vector<ComputationNodeBasePtr> DetermineOutputNodes(const std::vector<std::wstring>& outputNodeNames)
    {
        std::vector<ComputationNodeBasePtr> outputNodes;

        if (outputNodeNames.size() == 0)
        {
            if (m_verbosity > 0)
                fprintf(stderr, "OutputNodeNames are not specified, using the default outputnodes.\n");
            if (m_net->OutputNodes().size() == 0)
                LogicError("There is no default output node specified in the network.");

            outputNodes = m_net->OutputNodes();
        }
        else
        {
            for (int i = 0; i < outputNodeNames.size(); i++)
                outputNodes.push_back(m_net->GetNodeFromName(outputNodeNames[i]));
        }

        return outputNodes;
    }

    // collect all input nodes that outputNodes depend on
    // TODO: This is rather generic, we should move this to a shared place. DataReaderHelpers.h?
    std::vector<ComputationNodeBasePtr> DetermineInputNodes(const std::vector<ComputationNodeBasePtr>& outputNodes)
    {
        // use map to remove duplicated items
        std::set<ComputationNodeBasePtr> inputNodesMap;
        for (auto& onode : outputNodes)
        {
            for (auto& inode : m_net->InputNodes(onode))
                inputNodesMap.insert(inode);
        }

        std::vector<ComputationNodeBasePtr> inputNodes;
        for (auto& inode : inputNodesMap)
            inputNodes.push_back(inode);

        return inputNodes;
    }

    // get StreamMinibatchInputs for a given set of input nodes
    // TODO: This seems generic, we should have that in a shared place.
    StreamMinibatchInputs RetrieveInputMatrices(const std::vector<ComputationNodeBasePtr>& inputNodes)
    {
        StreamMinibatchInputs inputMatrices;
        for (auto& node : inputNodes)
            inputMatrices.AddInputMatrix(node->NodeName(), node->ValuePtr());
        return inputMatrices;
    }

public:
    SimpleOutputWriter(ComputationNetworkPtr net, int verbosity = 0)
        : m_net(net), m_verbosity(verbosity)
    {
    }

    void WriteOutput(IDataReader& dataReader, size_t mbSize, IDataWriter& dataWriter, const std::vector<std::wstring>& outputNodeNames, size_t numOutputSamples = requestDataSize, bool doUnitTest = false)
    {
        ScopedNetworkOperationMode modeGuard(m_net, NetworkOperationMode::inferring);

        std::vector<ComputationNodeBasePtr> outputNodes = DetermineOutputNodes(outputNodeNames);
        std::vector<ComputationNodeBasePtr> inputNodes  = DetermineInputNodes(outputNodes);

        // allocate memory for forward computation
        m_net->AllocateAllMatrices({}, outputNodes, nullptr);

        StreamMinibatchInputs inputMatrices = RetrieveInputMatrices(inputNodes);

        // evaluate with minibatches
        dataReader.StartMinibatchLoop(mbSize, 0, numOutputSamples);
        if (!dataWriter.SupportMultiUtterances())
            dataReader.SetNumParallelSequences(1);
        m_net->StartEvaluateMinibatchLoop(outputNodes);

        size_t totalEpochSamples = 0;
        std::map<std::wstring, void*, nocase_compare> outputMatrices;

        const size_t numIterationsBeforePrintingProgress = 100;
        size_t numItersSinceLastPrintOfProgress = 0;
        size_t actualMBSize;
        while (DataReaderHelpers::GetMinibatchIntoNetwork<ElemType>(dataReader, m_net, nullptr, false, false, inputMatrices, actualMBSize, nullptr))
        {
            ComputationNetwork::BumpEvalTimeStamp(inputNodes);

            for (int i = 0; i < outputNodes.size(); i++)
            {
                m_net->ForwardProp(outputNodes[i]);
                outputMatrices[outputNodes[i]->NodeName()] = (void*) (&dynamic_pointer_cast<ComputationNode<ElemType>>(outputNodes[i])->Value());
            }

            if (doUnitTest)
            {
                std::map<std::wstring, void*, nocase_compare> inputMatricesUnitTest;
                for (auto iter = inputMatrices.begin(); iter != inputMatrices.end(); iter++)
                    inputMatricesUnitTest[iter->first] = (void*) iter->second.get();  // BUGBUG: void* are evil
                dataWriter.SaveData(0, inputMatricesUnitTest, actualMBSize, actualMBSize, 0);
            }
            else
                dataWriter.SaveData(0, outputMatrices, actualMBSize, actualMBSize, 0);

            totalEpochSamples += actualMBSize;

            if (ProgressTracing::GetTracingFlag())
            {
                numItersSinceLastPrintOfProgress++;
                if (numItersSinceLastPrintOfProgress >= numIterationsBeforePrintingProgress)
                {
                    // TODO: For now just print 0.0 instead of calculating actual progress
                    printf("PROGRESS: %.2f%%\n", 0.0f);
                    numItersSinceLastPrintOfProgress = 0;
                }
            }

            // call DataEnd function in dataReader to do
            // reader specific process if sentence ending is reached
            dataReader.DataEnd();
        }

        if (m_verbosity > 0)
            fprintf(stderr, "Total Samples Evaluated = %lu\n", totalEpochSamples);

        // clean up
    }

    // Perform a single forward pass to obtain the output values from a network
    void WriteOutput(IDataWriter& dataWriter, const std::vector<std::wstring>& outputNodeNames, size_t numOutputSamples = requestDataSize, bool doUnitTest = false)
    {
        std::vector<ComputationNodeBasePtr> outputNodes = DetermineOutputNodes(outputNodeNames);

        // allocate memory for forward computation
        m_net->AllocateAllMatrices({}, outputNodes, nullptr);

        m_net->StartEvaluateMinibatchLoop(outputNodes);

        std::map<std::wstring, void*, nocase_compare> outputMatrices;

        for (int i = 0; i < outputNodes.size(); i++)
        {
            m_net->ForwardProp(outputNodes[i]);
            outputMatrices[outputNodes[i]->NodeName()] = (void*)(&dynamic_pointer_cast<ComputationNode<ElemType>>(outputNodes[i])->Value());
        }

        // TODO: What should the data size be?
        dataWriter.SaveData(0, outputMatrices, 1, 1, 0);
    }

    // pass this to WriteOutput() (to file-path, below) to specify how the output should be formatted
    struct WriteFormattingOptions
    {
        // How to interpret the data:
        bool isCategoryLabel;          // true: find max value in column and output the index instead of the entire vector
        std::wstring labelMappingFile; // optional dictionary for pretty-printing category labels
        bool transpose;                // true: one line per sample, each sample (column vector) forms one line; false: one column per sample
        // The following strings are interspersed with the data:
        // overall
        std::string prologue; // print this at the start (e.g. a global header or opening bracket)
        std::string epilogue; // and this at the end
        // sequences
        std::string sequenceSeparator; // print this between sequences (i.e. before all sequences but the first)
        std::string sequencePrologue;  // print this before each sequence (after sequenceSeparator)
        std::string sequenceEpilogue;  // and this after each sequence
        // elements
        std::string elementSeparator;  // print this between elements on a row
        std::string sampleSeparator;   // and this between rows
        // Optional printf precision parameter:
        std::string precisionFormat;        // printf precision, e.g. ".2" to get a "%.2f"

        WriteFormattingOptions() :
            isCategoryLabel(false), transpose(true), sequenceEpilogue("\n"), elementSeparator(" "), sampleSeparator("\n")
        { }

        // Process -- replace newlines and all %s by the given string
        static std::string Processed(const std::wstring& nodeName, std::string fragment)
        {
            fragment = msra::strfun::ReplaceAll<std::string>(fragment, "\\n", "\n");
            fragment = msra::strfun::ReplaceAll<std::string>(fragment, "\\t", "\t");
            if (fragment.find("%s") != fragment.npos)
                fragment = msra::strfun::ReplaceAll<std::string>(fragment, "%s", msra::strfun::utf8(nodeName));
            return fragment;
        }
    };

    // TODO: Remove code dup with above function by creating a fake Writer object and then calling the other function.
    void WriteOutput(IDataReader& dataReader, size_t mbSize, std::wstring outputPath, const std::vector<std::wstring>& outputNodeNames, const WriteFormattingOptions & formattingOptions, size_t numOutputSamples = requestDataSize)
    {
        ScopedNetworkOperationMode modeGuard(m_net, NetworkOperationMode::inferring);

        std::vector<ComputationNodeBasePtr> outputNodes = DetermineOutputNodes(outputNodeNames);
        std::vector<ComputationNodeBasePtr> inputNodes = DetermineInputNodes(outputNodes);

        // allocate memory for forward computation
        m_net->AllocateAllMatrices({}, outputNodes, nullptr);

        StreamMinibatchInputs inputMatrices = RetrieveInputMatrices(inputNodes);

        // load a label mapping if requested
        std::vector<std::string> labelMapping;
        if (formattingOptions.isCategoryLabel && !formattingOptions.labelMappingFile.empty())
            File::LoadLabelFile(formattingOptions.labelMappingFile, labelMapping);

        // open output files
        File::MakeIntermediateDirs(outputPath);
        std::map<ComputationNodeBasePtr, shared_ptr<File>> outputStreams; // TODO: why does unique_ptr not work here? Complains about non-existent default_delete()
        for (auto & onode : outputNodes)
        {
            std::wstring nodeOutputPath = outputPath;
            if (nodeOutputPath != L"-")
                nodeOutputPath += L"." + onode->NodeName();
            auto f = make_shared<File>(nodeOutputPath, fileOptionsWrite | fileOptionsText);
            outputStreams[onode] = f;
        }

        // evaluate with minibatches
        dataReader.StartMinibatchLoop(mbSize, 0, numOutputSamples);

        m_net->StartEvaluateMinibatchLoop(outputNodes);

        size_t totalEpochSamples = 0;
        size_t numMBsRun = 0;

        for (auto & onode : outputNodes)
        {
            FILE * f = *outputStreams[onode];
            fprintfOrDie(f, "%s", formattingOptions.prologue.c_str());
        }

        size_t actualMBSize;
        const size_t numIterationsBeforePrintingProgress = 100;
        size_t numItersSinceLastPrintOfProgress = 0;
        while (DataReaderHelpers::GetMinibatchIntoNetwork<ElemType>(dataReader, m_net, nullptr, false, false, inputMatrices, actualMBSize, nullptr))
        {
            ComputationNetwork::BumpEvalTimeStamp(inputNodes);

            for (auto & onode : outputNodes)
            {
                // compute the node value
                // Note: Intermediate values are memoized, so in case of multiple output nodes, we only compute what has not been computed already.
                m_net->ForwardProp(onode);

                // sequence separator
                FILE * f = *outputStreams[onode];
                const auto sequenceSeparator = formattingOptions.Processed(onode->NodeName(), formattingOptions.sequenceSeparator);
                const auto sequencePrologue  = formattingOptions.Processed(onode->NodeName(), formattingOptions.sequencePrologue);
                const auto sequenceEpilogue  = formattingOptions.Processed(onode->NodeName(), formattingOptions.sequenceEpilogue);
                const auto elementSeparator  = formattingOptions.Processed(onode->NodeName(), formattingOptions.elementSeparator);
                const auto sampleSeparator   = formattingOptions.Processed(onode->NodeName(), formattingOptions.sampleSeparator);

                char formatChar = !formattingOptions.isCategoryLabel ? 'f' : !formattingOptions.labelMappingFile.empty() ? 's' : 'u';
                std::string valueFormatString = "%" + formattingOptions.precisionFormat + formatChar; // format string used in fprintf() for formatting the values

                if (numMBsRun > 0) // WriteMinibatchWithFormatting() will not include this before first sequence
                    fprintfOrDie(f, "%s", sequenceSeparator.c_str());

                auto pnode = dynamic_pointer_cast<ComputationNode<ElemType>>(onode);
                pnode->WriteMinibatchWithFormatting(f, SIZE_MAX, SIZE_MAX, formattingOptions.transpose, formattingOptions.isCategoryLabel, labelMapping,
                                                    sequenceSeparator, sequencePrologue, sequenceEpilogue, elementSeparator, sampleSeparator,
                                                    valueFormatString);
            } // end loop over nodes

            totalEpochSamples += actualMBSize;

            fprintf(stderr, "Minibatch[%lu]: ActualMBSize = %lu\n", ++numMBsRun, actualMBSize);

            if (ProgressTracing::GetTracingFlag())
            {
                numItersSinceLastPrintOfProgress++;
                if (numItersSinceLastPrintOfProgress >= numIterationsBeforePrintingProgress)
                {
                    // TODO: For now just print 0.0 instead of calculating actual progress
                    printf("PROGRESS: %.2f%%\n", 0.0f);
                    numItersSinceLastPrintOfProgress = 0;
                }
            }

            // call DataEnd function in dataReader to do
            // reader specific process if sentence ending is reached
            dataReader.DataEnd();
        } // end loop over minibatches

        for (auto & onode : outputNodes)
        {
            FILE * f = *outputStreams[onode];
            fprintfOrDie(f, "%s", formattingOptions.epilogue.c_str());
        }

        fprintf(stderr, "Written to %ls*\nTotal Samples Evaluated = %lu\n", outputPath.c_str(), totalEpochSamples);

        // flush all files (where we can catch errors) so that we can then destruct the handle cleanly without error
        for (auto & iter : outputStreams)
            iter.second->Flush();
    }

private:
    ComputationNetworkPtr m_net;
    int m_verbosity;
    void operator=(const SimpleOutputWriter&); // (not assignable)
};

}}}
