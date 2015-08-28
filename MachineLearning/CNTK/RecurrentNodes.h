//
// <copyright file="RecurrentNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

#include "Basics.h"
#include "Matrix.h"
#include "ComputationNode.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    // =======================================================================
    // DelayedValueNode -- abstract base class for PastValueNode and FutureValueNode to hold all shared code
    // The two differ in the step direction, some loop directions, and sequence-boundary flags.
    // =======================================================================

    // TODO: make m_direction a template parameter
    template<class ElemType>
    class DelayedValueNode : public ComputationNode<ElemType>
    {
        UsingComputationNodeMembers;
    private:
        void Init(int direction, size_t row_size, size_t col_size, ElemType initialActivationValue = (ElemType)DEFAULT_HIDDEN_ACTIVITY)
        {
            m_reqMultiSeqHandling = true;
            m_initialActivationValue = initialActivationValue;
            m_timeStep = 1;
            m_functionValues.Resize(row_size, col_size);
            m_delayedActivation.Resize(row_size, col_size);

            m_direction = direction;    // TODO: make this a template parameter
            if (m_direction < 0)    // set flags what kind of sequence boundary we should trigger on
            {
                m_SEQUENCE_BOUNDARY = SEQUENCE_START;
                m_SequenceBoundary = MinibatchPackingFlag::SequenceStart;
            }
            else
            {
                m_SEQUENCE_BOUNDARY = SEQUENCE_END;
                m_SequenceBoundary = MinibatchPackingFlag::SequenceEnd;
            }
            m_historyAlreadySet = false;    // PastValueNode only
        }
    protected:
        virtual ComputationNode<ElemType> * NewThis(DEVICEID_TYPE deviceId, const wstring & name) = 0;
        DelayedValueNode(DEVICEID_TYPE deviceId, const wstring & name) : ComputationNode<ElemType>(deviceId, name)
        {
            // TODO: change this back to proper initializers
            m_delayedActivation = Matrix<ElemType>(deviceId), m_boundaryInfo = CPUDEVICE;
            Init(1000/*something bad--will go away once we make it a template parameter*/, 1, 1);
        }
        DelayedValueNode(DEVICEID_TYPE deviceId, const wstring & name, int direction) : ComputationNode<ElemType>(deviceId, name)
        {
            // TODO: change this back to proper initializers
            m_delayedActivation = Matrix<ElemType>(deviceId), m_boundaryInfo = CPUDEVICE;
            Init(direction, 1, 1);
        }
        DelayedValueNode(DEVICEID_TYPE deviceId, const wstring & name, int direction, ElemType initialActivationValue, size_t row_size, size_t col_size) : ComputationNode<ElemType>(deviceId, name)
        {
            // TODO: change this back to proper initializers
            m_delayedActivation = Matrix<ElemType>(deviceId), m_boundaryInfo = CPUDEVICE;
            Init(direction, row_size, col_size, initialActivationValue);

            m_functionValues.SetValue(m_initialActivationValue);
            m_delayedActivation.SetValue(m_initialActivationValue);

            m_gradientValues.Resize(row_size, col_size);
            m_gradientValues.SetValue(0.0f);
        }
    public:
        void SaveToFile(File& fstream) const
        {
            fstream << OperationName() << NodeName();
            fstream << m_timeStep;
            fstream << FunctionValues().GetNumRows() << FunctionValues().GetNumCols();

            fstream << m_initialActivationValue;
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion)
        {
            // the node has already been initialized e.g. w.r.t. direction and sequence flags
            ComputationNode<ElemType>::LoadFromFile(fstream, modelVersion);

            fstream >> m_timeStep;

            size_t iRow, timeIdxInSeq;
            fstream >> iRow >> timeIdxInSeq;
            FunctionValues().Resize(iRow, timeIdxInSeq);
            m_delayedActivation.Resize(iRow, timeIdxInSeq);

            if (modelVersion >= CNTK_MODEL_VERSION_2)
                fstream >> m_initialActivationValue;
        }

        virtual const std::wstring OperationName() const { return TypeName(); }
        static const std::wstring TypeName() { return L"DelayedValue"; }

        //Set sentence boundary information according to a specified time step. 
        void ResetBound(Matrix<ElemType> * seg, vector<MinibatchPackingFlag> * minibatchPackingFlag)
        {
            if (m_timeStep <= 0)
                LogicError("timeStep should be 1 or larger");

            ComputationNode<ElemType>::ResetBound(seg, minibatchPackingFlag);

            m_shiftedMinibatchPackingFlag = *minibatchPackingFlag;
            m_boundaryInfo = *seg;

            if (m_timeStep > 1)
            {
                //each row has a number to indicate how many values should be reset for that utterance
                int numRows = (int)seg->GetNumRows();
                vector<int> numResetLeft;
                numResetLeft.resize(numRows);
                std::fill(numResetLeft.begin(), numResetLeft.end(), 0);

                // Note: this loop direction is from PastValueNode. FutureValueNode uses this for loop:
                //   for (int i = minibatchPackingFlag->size() - 1; i >= 0; i--) // Future
                // The loop bodies for each 'i' are independent, so the loop order should not matter; hence, this code can be shared.
                for (int i = 0; i < minibatchPackingFlag->size(); i++)      // Past
                {
                    if ((*minibatchPackingFlag)[i] & (m_SequenceBoundary | MinibatchPackingFlag::NoFeature))
                    {
                        //we set timeStep-1 elements following it to be SequenceStart until met NoInput
                        for (int j = 0; j < numRows; j++)
                        {
                            //we use & since SEQUENCE_START may come with NoLabel
                            if ((int)(*seg)(j, i) & m_SEQUENCE_BOUNDARY)
                                numResetLeft[j] = m_timeStep;
                            else if ((int)(*seg)(j, i) & NO_FEATURE)
                                numResetLeft[j] = 0;
                        }
                    }

                    //now set the sequence-boundary flag
                    bool valueChanged = false;
                    for (int j = 0; j < numRows; j++)
                    {
                        if (numResetLeft[j]-- > 0)
                        {
                            m_boundaryInfo(j, i) = (ElemType)(m_SEQUENCE_BOUNDARY | ((int)m_boundaryInfo(j, i) & NO_LABEL));
                            valueChanged = true;
                        }
                    }

                    if (valueChanged)
                        m_shiftedMinibatchPackingFlag[i] |= m_SequenceBoundary;
                }
            }
        }

        // this one differs in loop direction
        virtual void ComputeInputPartial(const size_t inputIndex) = 0;

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange)
        {
            if (inputIndex > 0)
                InvalidArgument("PastValue and FutureValue operations only take one input.");

            assert(m_functionValues.GetNumRows() == GradientValues().GetNumRows());
            assert(m_sentenceSeg != nullptr);
            assert(m_minibatchPackingFlag != nullptr);

            Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(frameRange.t(), 1);

            ComputeInputPartialSRP(frameRange.t(), m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, Inputs(0)->GradientValues(), GradientValues(), m_samplesInRecurrentStep, colBoundaryFlags, m_shiftedMinibatchPackingFlag[frameRange.t()]);
        }

        static void WINAPI ComputeInputPartialSRP(int timeIdxInSeq, int timeStep, int direction, MinibatchPackingFlag SequenceBoundary, int SEQUENCE_BOUNDARY,
                                                  Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues, const size_t mNbr,
                                                  const Matrix<ElemType>& colBoundaryFlags, MinibatchPackingFlag minibatchPackingFlag)
        {
            assert(timeIdxInSeq >= 0);
            if (timeIdxInSeq + direction * timeStep >= 0 && timeIdxInSeq + direction * timeStep < gradientValues.GetNumCols())
            {
                if (minibatchPackingFlag & (SequenceBoundary | MinibatchPackingFlag::NoFeature))
                {
                    for (int i = 0; i < mNbr; i++)
                    {
                        if (! ((int)colBoundaryFlags(i,0) & SEQUENCE_BOUNDARY) &&
                            ! ((int)colBoundaryFlags(i,0) & NO_FEATURE))
                        {
                            Matrix<ElemType> to = inputGradientValues.ColumnSlice((timeIdxInSeq + direction * timeStep)*mNbr + i, 1);
                            Matrix<ElemType> frm = gradientValues.ColumnSlice(timeIdxInSeq * mNbr + i, 1);
                            to += frm;
                        }
                    }

                }
                else
                {
                    Matrix<ElemType> frm = gradientValues.ColumnSlice(timeIdxInSeq * mNbr, mNbr);
                    Matrix<ElemType> to = inputGradientValues.ColumnSlice((timeIdxInSeq + direction * timeStep)*mNbr, mNbr);
                    to += frm;
                }
            }
        }

        // this one differs in loop direction
        virtual void EvaluateThisNode() = 0;

        // this one differs in the starting condition
        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) = 0;

        static void WINAPI EvaluateThisNodeSRP(const size_t timeIdxInSeq, const int timeStep, const int direction, MinibatchPackingFlag SequenceBoundary, int SEQUENCE_BOUNDARY,
                                                Matrix<ElemType>& functionValues, const Matrix<ElemType>& delayedActivation, const Matrix<ElemType>& inputFunctionValues, const size_t mNbr,
                                                const ElemType & initStateValue, const Matrix<ElemType> & colBoundaryFlags, const MinibatchPackingFlag minibatchPackingFlag)
        {
            ASSERT(timeStep > 0);

            if (functionValues.GetNumRows() != inputFunctionValues.GetNumRows() ||
                functionValues.GetNumCols() != inputFunctionValues.GetNumCols())
                functionValues.Resize(inputFunctionValues.GetNumRows(),
                inputFunctionValues.GetNumCols());

            int delayedIndex = (int)(timeIdxInSeq + direction * timeStep) * mNbr;
            int d = delayedIndex;
            if (d < 0 || d >= inputFunctionValues.GetNumCols())
                d = (int)functionValues.Mod((float)delayedIndex, (float)delayedActivation.GetNumCols());
            // this can point to the past activity of the previous mninibatch

            Matrix<ElemType> out = functionValues.ColumnSlice(timeIdxInSeq * mNbr, mNbr);
            Matrix<ElemType> inp((DEVICEID_TYPE)functionValues.GetDeviceId());

            if (minibatchPackingFlag & SequenceBoundary)
            {
                for (int i = 0; i < mNbr; i++)
                {
                    out = functionValues.ColumnSlice(timeIdxInSeq * mNbr + i, 1);

                    if ((int)colBoundaryFlags(i,0) & SEQUENCE_BOUNDARY)
                        out.SetValue(initStateValue);
                    else
                    {
                        if (delayedIndex < 0 || delayedIndex >= inputFunctionValues.GetNumCols())
                            inp = delayedActivation.ColumnSlice(d + i, 1);
                        else
                            inp = inputFunctionValues.ColumnSlice(d + i, 1);

                        out.SetValue(inp);
                    }
                }
            }
            else
            {
                if (delayedIndex < 0 || delayedIndex >= inputFunctionValues.GetNumCols())
                    inp = delayedActivation.ColumnSlice(d, mNbr);
                else
                    inp = inputFunctionValues.ColumnSlice(d, mNbr);

                out.SetValue(inp);
            }
        }

        virtual void Validate()
        {
            PrintSelfBeforeValidation(true/*allowNulls*/);

            if (m_children.size() != 1)
                throw std::logic_error("PastValue operation should have one input.");

            if (!(Inputs(0) == nullptr))
            {
                size_t rows0 = Inputs(0)->FunctionValues().GetNumRows(),
                    cols0 = Inputs(0)->FunctionValues().GetNumCols();

                if (rows0 > 0 && cols0 > 0) FunctionValues().Resize(rows0, cols0);
            }
            InferImageDimsFromInputs();
        }

        // the following two are only used for PastValueNode
        bool GetHistory(Matrix<ElemType>& hist, bool)
        {
            DEVICEID_TYPE device = hist.GetDeviceId();
            hist.TransferFromDeviceToDevice(device, m_deviceId, true);

            hist.SetValue(Inputs(0)->FunctionValues());

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
            return true;
        }

        void SetHistory(const Matrix<ElemType>& hist)
        {
            DEVICEID_TYPE device = hist.GetDeviceId();
            hist.TransferFromDeviceToDevice(device, m_deviceId, true);

            m_delayedActivation.SetValue(hist);
            m_historyAlreadySet = true;

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
        }

        virtual void AttachInputs(const ComputationNodePtr inputNode)
        {
            m_children.resize(1);
            m_children[0] = inputNode;
        }

        void SetTimeStep(const int val)
        {
            if (val <= 0)
                throw std::logic_error("timeStep must be > 0.");
            m_timeStep = val;
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            ComputationNode<ElemType>::MoveMatricesToDevice(deviceId);
            m_boundaryInfo.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_delayedActivation.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            ComputationNode<ElemType>::CopyTo(nodeP, newName, flags);
            auto node = dynamic_pointer_cast<DelayedValueNode<ElemType>>(nodeP);

            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_timeStep = m_timeStep;
                node->m_initialActivationValue = m_initialActivationValue;
                node->m_delayedActivation = m_delayedActivation;
                node->m_direction = m_direction;
                node->m_SEQUENCE_BOUNDARY = m_SEQUENCE_BOUNDARY;
                node->m_SequenceBoundary = m_SequenceBoundary;
                node->m_historyAlreadySet = false;
            }
        }

        // copy constructor
        void Construct(const DelayedValueNode<ElemType>* node, const std::wstring& newName, const CopyNodeFlags flags)
        {
            //DELETETHIS ComputationNode<ElemType>::Construct(node->m_deviceId, newName);
            node->CopyTo(shared_from_this(), newName, flags);
        }

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const = 0;

    protected:
        virtual bool UseCustomizedMultiSeqHandling() { return true; }

    protected:
        ElemType m_initialActivationValue;      // starting value for hidden activation vector at boundary
        Matrix<ElemType> m_delayedActivation;   // saves the activation of the previous step that this node points to
        int      m_timeStep;                    // delay in frames (typ. 1)
        int      m_direction;                   // +1 for FutureValueNode, -1 for PastValueNode
        int      m_SEQUENCE_BOUNDARY;           // MinibatchPackingFlag SEQUENCE_END for FutureValueNode or SEQUENCE_START for PastValueNode
        MinibatchPackingFlag m_SequenceBoundary;// SequenceEnd for FutureValueNode or SequenceStart for PastValueNode
        vector<MinibatchPackingFlag> m_shiftedMinibatchPackingFlag;
        Matrix<ElemType> m_boundaryInfo;        // individual sentence boundary information 
        bool m_historyAlreadySet;               // for PastValueNode only
    };

#define UsingDelayedValueNodeMembers UsingComputationNodeMembers; typedef DelayedValueNode<ElemType> BB; \
public:  \
    using BB::Construct; using BB::m_initialActivationValue; using BB::m_delayedActivation; using BB::m_timeStep; using BB::m_direction; \
    using BB::m_SEQUENCE_BOUNDARY; using BB::m_SequenceBoundary; using BB::m_shiftedMinibatchPackingFlag; using BB::m_boundaryInfo; using BB::m_historyAlreadySet; \
    using BB::ComputeInputPartialSRP; using BB::EvaluateThisNodeSRP

    // =======================================================================
    // PastValueNode -- delay node
    // =======================================================================

    template<class ElemType>
    class PastValueNode : public DelayedValueNode<ElemType>
    {
        UsingDelayedValueNodeMembers;
    public:
        virtual ComputationNode<ElemType> * NewThis(DEVICEID_TYPE deviceId, const wstring & name) { return new std::remove_reference<decltype(*this)>::type(deviceId, name); }
        PastValueNode(DEVICEID_TYPE deviceId, const wstring & name) : DelayedValueNode<ElemType>(deviceId, name, -1)
        {
            // TODO: change this back to proper initializers
        }
        PastValueNode(DEVICEID_TYPE deviceId, const wstring & name, ElemType initialActivationValue, size_t row_size, size_t col_size) : DelayedValueNode<ElemType>(deviceId, name, -1, initialActivationValue, row_size, col_size)
        {
            // TODO: change this back to proper initializers
        }

        virtual const std::wstring OperationName() const { return TypeName(); }
        static const std::wstring TypeName() { return L"PastValue"; }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 0)
                InvalidArgument("PastValue and FutureValue operations only take one input.");

            int nbrSamples = GradientValues().GetNumCols() / m_samplesInRecurrentStep; 
            for (int timeIdxInSeq = nbrSamples - 1; timeIdxInSeq >= 0; timeIdxInSeq--)
            {
                // TODO: call the looping version below to avoid code dup
                Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(timeIdxInSeq, 1);
            
                ComputeInputPartialSRP(timeIdxInSeq, m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, Inputs(0)->GradientValues(), GradientValues(), m_samplesInRecurrentStep, colBoundaryFlags, m_shiftedMinibatchPackingFlag[timeIdxInSeq]);
            }
        }

        // TODO: why is this loop not in th underlying execution engine? This node should not have to know about this.
        virtual void EvaluateThisNode()  
        {
            ASSERT(m_timeStep > 0);
            int blockSize = Inputs(0)->FunctionValues().GetNumCols();

            for (int timeIdxInSeq = 0; timeIdxInSeq < blockSize / m_samplesInRecurrentStep; timeIdxInSeq++)
            {
                // TODO: call the looping version below to avoid code dup
                Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(timeIdxInSeq, 1);
                EvaluateThisNodeSRP(timeIdxInSeq, m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, m_functionValues, m_delayedActivation, Inputs(0)->FunctionValues(), m_samplesInRecurrentStep, m_initialActivationValue, colBoundaryFlags, m_shiftedMinibatchPackingFlag[timeIdxInSeq]);
            }

            //set the past activity to be used by next minibatch
            m_delayedActivation = Inputs(0)->FunctionValues();
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange)  
        {
            // reset past activity as it reached to the begining of a minibatch
            // the node pointed hasn't yet updated, so it is the past activity 
            assert(m_sentenceSeg != nullptr);
            assert(m_minibatchPackingFlag != nullptr);

            if (frameRange.t() == 0 && m_historyAlreadySet == false)
                m_delayedActivation = Inputs(0)->FunctionValues();
            
            Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(frameRange.t(), 1);
            EvaluateThisNodeSRP(frameRange.t(), m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, m_functionValues, m_delayedActivation, Inputs(0)->FunctionValues(), m_samplesInRecurrentStep, m_initialActivationValue, colBoundaryFlags, m_shiftedMinibatchPackingFlag[frameRange.t()]);
        }

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const
        {
            const std::wstring& name = (newName == L"") ? NodeName() : newName;
            return New<PastValueNode<ElemType>>(this, name, flags);
        }
    };

    template class PastValueNode<float>; 
    template class PastValueNode<double>;


    // =======================================================================
    // FutureValueNode -- delay node in future direction
    // =======================================================================

    //get value from future (used in the bi-directional models)
    template<class ElemType>
    class FutureValueNode : public DelayedValueNode<ElemType>
    {
        UsingDelayedValueNodeMembers;
    public:
        virtual ComputationNode<ElemType> * NewThis(DEVICEID_TYPE deviceId, const wstring & name) { return new std::remove_reference<decltype(*this)>::type(deviceId, name); }
        FutureValueNode(DEVICEID_TYPE deviceId, const wstring & name) : DelayedValueNode<ElemType>(deviceId, name, +1)
        {
            // TODO: change this back to proper initializers
        }
        FutureValueNode(DEVICEID_TYPE deviceId, const wstring & name, ElemType initialActivationValue, size_t row_size, size_t col_size) : DelayedValueNode<ElemType>(deviceId, name, +1, initialActivationValue, row_size, col_size)
        {
            // TODO: change this back to proper initializers
        }

        virtual const std::wstring OperationName() const { return TypeName(); }
        static const std::wstring TypeName() { return L"FutureValue"; }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 0)
                InvalidArgument("PastValue and FutureValue operations only take one input.");

            int nbrSamples = GradientValues().GetNumCols() / m_samplesInRecurrentStep;
            for (int timeIdxInSeq = 0; timeIdxInSeq < nbrSamples; timeIdxInSeq++)
            {
                // TODO: call the looping version below to avoid code dup
                Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(timeIdxInSeq, 1);

                ComputeInputPartialSRP(timeIdxInSeq, m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, Inputs(0)->GradientValues(), GradientValues(), m_samplesInRecurrentStep, colBoundaryFlags, m_shiftedMinibatchPackingFlag[timeIdxInSeq]);
            }
        }

        virtual void EvaluateThisNode()
        {
            ASSERT(m_timeStep > 0);
            int blockSize = Inputs(0)->FunctionValues().GetNumCols();

            for (int timeIdxInSeq = blockSize / m_samplesInRecurrentStep - 1; timeIdxInSeq >= 0; timeIdxInSeq--)
            {
                Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(timeIdxInSeq, 1);
                EvaluateThisNodeSRP(timeIdxInSeq, m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, m_functionValues, m_delayedActivation, Inputs(0)->FunctionValues(), m_samplesInRecurrentStep, m_initialActivationValue, colBoundaryFlags, m_shiftedMinibatchPackingFlag[timeIdxInSeq]);
            }

            //set the future activity to be used by next minibatch
            m_delayedActivation = Inputs(0)->FunctionValues();
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange)
        {
            assert(m_sentenceSeg != nullptr);
            assert(m_minibatchPackingFlag != nullptr);

            if (frameRange.t() == Inputs(0)->FunctionValues().GetNumCols() / m_samplesInRecurrentStep - 1)
                m_delayedActivation = Inputs(0)->FunctionValues();

            Matrix<ElemType> colBoundaryFlags = m_boundaryInfo.ColumnSlice(frameRange.t(), 1);
            EvaluateThisNodeSRP(frameRange.t(), m_timeStep, m_direction, m_SequenceBoundary, m_SEQUENCE_BOUNDARY, m_functionValues, m_delayedActivation, Inputs(0)->FunctionValues(), m_samplesInRecurrentStep, m_initialActivationValue, colBoundaryFlags, m_shiftedMinibatchPackingFlag[frameRange.t()]);
        }

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const
        {
            const std::wstring& name = (newName == L"") ? NodeName() : newName;
            return New<FutureValueNode<ElemType>>(this, name, flags);
        }
    };

    template class FutureValueNode<float>;
    template class FutureValueNode<double>;


    // =======================================================================
    // LSTMNode -- deprecated early implementation of LSTM operating on minibatches directly
    // =======================================================================

    /**
    LSTM specific node. This node uses matrix operations to have LSTM functionality. 
    It avoids using general recurrent loop operations in the network operations in computationnetwork. 

    Developed by Kaisheng Yao
    Used in the following works:
    K. Yao, G. Zweig, "Sequence to sequence neural net models for graphone to phoneme conversion", in Interspeech 2015
    */
    template<class ElemType>
    class LSTMNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>
    {
        UsingComputationNodeMembers;

        void Init()
        {
            m_reqMultiSeqHandling = true;
            m_inputDim = 0;
            m_outputDim = 0;
            m_use_errors_from_future_minibatch = false;
            m_DefaultState = (ElemType)DEFAULT_HIDDEN_ACTIVITY;
        }
    public:
        virtual ComputationNode<ElemType> * NewThis(DEVICEID_TYPE deviceId, const wstring & name) { return new std::remove_reference<decltype(*this)>::type(deviceId, name); }
        LSTMNode(DEVICEID_TYPE deviceId, const wstring & name) : ComputationNodeNonLooping<ElemType>(deviceId, name)
        {
            m_State = Matrix<ElemType>(deviceId), m_PastState = Matrix<ElemType>(deviceId);
            m_PastOutput = Matrix<ElemType>(deviceId), m_Gi = Matrix<ElemType>(deviceId), m_Gf = Matrix<ElemType>(deviceId), m_Go = Matrix<ElemType>(deviceId); grdToObs = Matrix<ElemType>(deviceId); grdToInputGate = Matrix<ElemType>(deviceId);
            grdToForgetGate = Matrix<ElemType>(deviceId); grdToOutputGate = Matrix<ElemType>(deviceId); grdToCellWgt = Matrix<ElemType>(deviceId); tanhObs = Matrix<ElemType>(deviceId);
            tanhState = Matrix<ElemType>(deviceId), m_tempMatrix = Matrix<ElemType>(deviceId);
            mSlicePrevState = Matrix<ElemType>(deviceId); mSlicePrevOutput = Matrix<ElemType>(deviceId);
            grdBeforeInputGate = Matrix<ElemType>(deviceId);
            grdBeforeForget = Matrix<ElemType>(deviceId); grdBeforeGo = Matrix<ElemType>(deviceId); grdToCell = Matrix<ElemType>(deviceId);
            grdBeforeTanhInputGate = Matrix<ElemType>(deviceId), m_obs_error_from_future_minibatch = Matrix<ElemType>(deviceId);
            m_state_error_from_future_minibatch = Matrix<ElemType>(deviceId); mLastState = Matrix<ElemType>(deviceId); mLastOutput = Matrix<ElemType>(deviceId);
            Init(); // TODO: remove Init() as a function if only used in one place
        }

        // copy constructor
        void Construct(const LSTMNode<ElemType>* node, const std::wstring& newName, const CopyNodeFlags flags)
        {
            //DELETETHIS ComputationNode<ElemType>::Construct(node->m_deviceId, newName);
            node->CopyTo(shared_from_this(), newName, flags);
            m_DefaultState = (ElemType) DEFAULT_HIDDEN_ACTIVITY;
        }
        // TODO: we need CopyTo() that sets m_DefaultState to Default

        virtual ComputationNodePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) const
        {
            const std::wstring& name = (newName == L"") ? NodeName() : newName;
            return New<LSTMNode<ElemType>>(this, name, flags);
        }

        virtual const std::wstring OperationName() const { return TypeName(); }
        static const std::wstring TypeName() { return L"LSTM"; }

        virtual void SaveToFile(File& fstream) const
        {
            ComputationNode<ElemType>::SaveToFile(fstream);
            fstream << m_inputDim << m_outputDim;
            fstream << m_DefaultState;
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion)
        {
            ComputationNode<ElemType>::LoadFromFile(fstream, modelVersion);
            if (modelVersion == 2)
                fstream >> m_inputDim >> m_outputDim;
            fstream >> m_DefaultState;
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            ComputationNode<ElemType>::CopyTo(nodeP, newName, flags);
            auto node = dynamic_pointer_cast<LSTMNode<ElemType>>(nodeP);

            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_inputDim = m_inputDim;
                node->m_outputDim = m_outputDim;

                node->m_State = m_State;  // hidden state activity
                node->m_PastState = m_PastState; // state activity in the previous minibatch
                node->m_PastOutput = m_PastOutput; // output in the previou minibatch 

                node->m_Gi = m_Gi;     // input gate activity
                node->m_Gf = m_Gf;     // forget gate activity
                node->m_Go = m_Go;     // output gate activity

                node->mSlicePrevOutput = mSlicePrevOutput;
                node->mSlicePrevState = mSlicePrevState;

                node->m_use_errors_from_future_minibatch = m_use_errors_from_future_minibatch;

                node->m_DefaultState = m_DefaultState;
                node->m_reqMultiSeqHandling = m_reqMultiSeqHandling;
            }
        }

        virtual void ComputeInputPartial(const size_t inputIndex)
        {
            if (inputIndex > 4)
                InvalidArgument("LSTM operation only takes five inputs.");

            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            size_t inputDim = Inputs(0)->FunctionValues().GetNumRows();
            size_t outputDim = Inputs(1)->FunctionValues().GetNumRows();

            if (m_GradientComputed == false)
            {
                if (FunctionValues().GetNumCols() != GradientValues().GetNumCols() ||
                    FunctionValues().GetNumRows() != GradientValues().GetNumRows())
                {
                    throw std::runtime_error("LSTMNode::GradientValue size doesn't match to the function value size");
                }

                // reset gradients
                grdToObs.Resize(inputDim, nT); grdToObs.SetValue(0);
                grdToInputGate.Resize(Inputs(1)->FunctionValues().GetNumRows(), Inputs(1)->FunctionValues().GetNumCols()); grdToInputGate.SetValue(0);
                grdToForgetGate.Resize(Inputs(2)->FunctionValues().GetNumRows(), Inputs(2)->FunctionValues().GetNumCols()); grdToForgetGate.SetValue(0);
                grdToOutputGate.Resize(Inputs(3)->FunctionValues().GetNumRows(), Inputs(3)->FunctionValues().GetNumCols()); grdToOutputGate.SetValue(0);
                grdToCellWgt.Resize(Inputs(4)->FunctionValues().GetNumRows(), Inputs(4)->FunctionValues().GetNumCols()); grdToCellWgt.SetValue(0);

                Matrix<ElemType> slicePrevOutput(m_deviceId), slicePrevState(m_deviceId);
                Matrix<ElemType> grdToPrevOutput(m_deviceId), grdToPrevState(m_deviceId);
                Matrix<ElemType> stateError(m_deviceId);
                slicePrevState.Resize(outputDim, m_samplesInRecurrentStep);
                slicePrevOutput.Resize(outputDim, m_samplesInRecurrentStep);
                slicePrevOutput.SetValue(0);

                stateError.Resize(slicePrevState.GetNumRows(), slicePrevState.GetNumCols());

                grdToPrevOutput.Resize(slicePrevOutput.GetNumRows(), slicePrevOutput.GetNumCols());
                grdToPrevState.Resize(slicePrevState.GetNumRows(), slicePrevState.GetNumCols());
                grdToPrevOutput.SetValue(0);
                grdToPrevState.SetValue(0);

                for (int timeIdxInSeq = nT - m_samplesInRecurrentStep; timeIdxInSeq >= 0; timeIdxInSeq -= m_samplesInRecurrentStep)
                {
                    Matrix<ElemType> sliceObs = Inputs(0)->FunctionValues().ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceOutput = FunctionValues().ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceState = m_State.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);

                    Matrix<ElemType> sliceGi = m_Gi.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceGf = m_Gf.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceGo = m_Go.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);

                    Matrix<ElemType> sliceTanhState = tanhState.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceTanhObs = tanhObs.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);

                    Matrix<ElemType> error = GradientValues().ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep);

                    Matrix<ElemType> grdToObsSlice(this->m_deviceId);


#ifdef DEBUG_DECODER
                    fprintf(stderr, "original output error [%ld] norm = %.8e\n", timeIdxInSeq, error.FrobeniusNorm());
#endif

                    PrepareThisErrorsBeforeBackProp(timeIdxInSeq, nT, error, stateError, grdToPrevOutput, grdToPrevState,
                        m_obs_error_from_future_minibatch, m_state_error_from_future_minibatch, m_samplesInRecurrentStep, m_sentenceSeg);

#ifdef DEBUG_DECODER
                    fprintf(stderr, "output error [%ld] norm = %.8e\n", timeIdxInSeq, error.FrobeniusNorm());
                    fprintf(stderr, "state error [%ld] norm = %.8e\n", timeIdxInSeq, stateError.FrobeniusNorm());
#endif

                    grdToPrevOutput.Resize(slicePrevOutput.GetNumRows(), slicePrevOutput.GetNumCols());
                    grdToPrevState.Resize(slicePrevState.GetNumRows(), slicePrevState.GetNumCols());
                    grdToPrevOutput.SetValue(0);
                    grdToPrevState.SetValue(0);

                    PrepareHistory(timeIdxInSeq, mSlicePrevOutput, mSlicePrevState, FunctionValues(), m_State, m_PastOutput, m_PastState, m_samplesInRecurrentStep, m_DefaultState, m_sentenceSeg);

                    ComputeInputGradientWrtGates(
                        error,
                        sliceObs,
                        grdToObsSlice,
                        Inputs(1)->FunctionValues(),
                        grdToInputGate,
                        Inputs(2)->FunctionValues(),
                        grdToForgetGate,
                        Inputs(3)->FunctionValues(),
                        grdToOutputGate,
                        Inputs(4)->FunctionValues(),
                        grdToCellWgt,
                        mSlicePrevOutput,
                        mSlicePrevState,
                        stateError,
                        sliceState,
                        sliceTanhState,
                        sliceTanhObs,
                        sliceGi,
                        sliceGf,
                        sliceGo,
                        grdToPrevOutput,
                        grdToPrevState,
                        m_tempMatrix
                    );
                    grdToObs.ColumnSlice(timeIdxInSeq, m_samplesInRecurrentStep).SetValue(grdToObsSlice);

                    PrepareErrors(timeIdxInSeq, grdToPrevOutput, grdToPrevState, m_samplesInRecurrentStep, m_sentenceSeg);
                }
#ifdef DEBUG_DECODER
                fprintf(stderr, "after error prop b_c norm = %.8e\n", Inputs(4)->FunctionValues().ColumnSlice(0, 1).FrobeniusNorm());
#endif
                m_obs_error_from_future_minibatch = grdToPrevOutput;
                m_state_error_from_future_minibatch = grdToPrevState;


#ifdef DEBUG_DECODER
                fprintf(stderr, "pass error to encoder error = %.4e state error = %.4e\n", m_obs_error_from_future_minibatch.FrobeniusNorm(), m_state_error_from_future_minibatch.FrobeniusNorm());
#endif
                m_GradientComputed = true;
            }

            if (inputIndex == 0)  //derivative with regard to the observation
            {
                if (Inputs(inputIndex)->GradientValues().HasNoElements())
                    Inputs(inputIndex)->GradientValues().SetValue(grdToObs);
                else
                    Inputs(inputIndex)->GradientValues() += grdToObs;
            }

            if (inputIndex == 1)
            {
                if (Inputs(inputIndex)->GradientValues().HasNoElements())
                    Inputs(inputIndex)->GradientValues().SetValue(grdToInputGate);
                else
                    Inputs(inputIndex)->GradientValues() += grdToInputGate;
            }

            if (inputIndex == 2)
            {
                if (Inputs(inputIndex)->GradientValues().HasNoElements())
                    Inputs(inputIndex)->GradientValues().SetValue(grdToForgetGate);
                else
                    Inputs(inputIndex)->GradientValues() += grdToForgetGate;
            }

            if (inputIndex == 3)
            {
                if (Inputs(inputIndex)->GradientValues().HasNoElements())
                    Inputs(inputIndex)->GradientValues().SetValue(grdToOutputGate);
                else
                    Inputs(inputIndex)->GradientValues() += grdToOutputGate;
            }

            if (inputIndex == 4)
            {
                if (Inputs(inputIndex)->GradientValues().HasNoElements())
                    Inputs(inputIndex)->GradientValues().SetValue(grdToCellWgt);
                else
                    Inputs(inputIndex)->GradientValues() += grdToCellWgt;
            }
#ifdef DEBUG_DECODER
            fprintf(stderr, "LSTM gradient[%d] norm = %.8e\n", inputIndex, Inputs(inputIndex)->GradientValues().FrobeniusNorm());
#endif

        }

        static void WINAPI GradientOfTanh(const Matrix<ElemType>& functionValues,
            const Matrix<ElemType>& gradientOut,
            Matrix<ElemType>& inputGradientValues,
            Matrix<ElemType>& extTmp)
        {
            Matrix<ElemType> mTmp(inputGradientValues.GetDeviceId());
            extTmp.AssignElementProductOf(functionValues, functionValues); // v .* v
            mTmp.AssignDifferenceOf(1, extTmp); // 1-v^2
            if (inputGradientValues.GetNumRows() != functionValues.GetNumRows() ||
                inputGradientValues.GetNumCols() != functionValues.GetNumCols())
                throw std::logic_error("LSTMNode::GradientOfTanh : inputGradientValues need to be pre-allocated!");
            inputGradientValues.AddElementProductOf(gradientOut, mTmp); //  d .* ((1-v) .* v))
        }

        static void WINAPI ComputeInputGradientWrtGates(
            const Matrix<ElemType>& outGrd,  // the error to h_t from upper layer
            const Matrix<ElemType> & obs,
            Matrix<ElemType> &grdToObs,
            const Matrix<ElemType>& mInputGate,
            Matrix<ElemType> &grdToInputGate,
            const Matrix<ElemType> &mForgetGate,
            Matrix<ElemType> &grdToForgetGate,
            const Matrix<ElemType> &mOutputGate,
            Matrix<ElemType>& grdToOutputGate,
            const Matrix<ElemType> &mCellWgt,
            Matrix<ElemType> &grdToCellWgt,
            const Matrix<ElemType>& prevOutput,
            const Matrix<ElemType>& prevState,
            const Matrix<ElemType>& stateError,  // the error propagated to cell from t+1
            const Matrix<ElemType> &state,
            const Matrix<ElemType> &tanhState,
            const Matrix<ElemType> & tanhBeforeApplyingInputGating,
            const Matrix<ElemType> &gi,
            const Matrix<ElemType> &gf,
            const Matrix<ElemType> &go,
            Matrix<ElemType> &grdToPrevOutput,
            Matrix<ElemType> &grdToPrevState,
            Matrix<ElemType> & tmpMat
            )
        {
            int inputDim = obs.GetNumRows();
            int outputDim = mOutputGate.GetNumRows();

            assert(grdToPrevOutput.FrobeniusNorm() == 0);
            assert(grdToPrevState.FrobeniusNorm() == 0);
            assert(state.FrobeniusNorm() > 0);
            Matrix<ElemType> Who = mOutputGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> Wco = mOutputGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> Wxo = mOutputGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdToWho = grdToOutputGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> grdToWco = grdToOutputGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> grdToWxo = grdToOutputGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdTobo = grdToOutputGate.ColumnSlice(0, 1);

            Matrix<ElemType> Whf = mForgetGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> Wcf = mForgetGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> Wxf = mForgetGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdToWhf = grdToForgetGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> grdToWcf = grdToForgetGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> grdToWxf = grdToForgetGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdTobf = grdToForgetGate.ColumnSlice(0, 1);

            Matrix<ElemType> Wxc = mCellWgt.ColumnSlice(1, inputDim);
            Matrix<ElemType> Whc = mCellWgt.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> grdToWxc = grdToCellWgt.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdToWhc = grdToCellWgt.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> grdTobc = grdToCellWgt.ColumnSlice(0, 1);

            Matrix<ElemType> Whi = mInputGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> Wci = mInputGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> Wxi = mInputGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdToWhi = grdToInputGate.ColumnSlice(1 + inputDim, outputDim);
            Matrix<ElemType> grdToWci = grdToInputGate.ColumnSlice(1 + inputDim + outputDim, 1);
            Matrix<ElemType> grdToWxi = grdToInputGate.ColumnSlice(1, inputDim);
            Matrix<ElemType> grdTobi = grdToInputGate.ColumnSlice(0, 1);

            // error backpropagate to output gate
            Matrix<ElemType> grdToGo(tmpMat.GetDeviceId()), gradientOfSigmoid(tmpMat.GetDeviceId());
            Matrix<ElemType> grdBeforeGo(tmpMat.GetDeviceId()), grdBeforeInputGate(tmpMat.GetDeviceId());
            Matrix<ElemType> grdToCell(tmpMat.GetDeviceId());

            tmpMat.AssignElementProductOf(outGrd, tanhState);  // error to o_t
            gradientOfSigmoid.AssignSigmoidDerivativeOf(go);
            grdBeforeGo.AssignElementProductOf(tmpMat, gradientOfSigmoid);  // error before softmax
#ifdef DEBUG_DECODER
            fprintf(stderr, "output gate error = %.4e\n", grdBeforeGo(0, 0));
#endif
            Matrix<ElemType>::MultiplyAndAdd(Who, true, grdBeforeGo, false, grdToPrevOutput);  // error to previous output
            Matrix<ElemType>::MultiplyAndAdd(Wxo, true, grdBeforeGo, false, grdToObs);      // error to observation 
            tmpMat = grdBeforeGo;
            tmpMat.ColumnElementMultiplyWith(Wco);
            grdToCell = tmpMat;                                                            // error to memory cell

            Matrix<ElemType>::MultiplyAndAdd(grdBeforeGo, false, prevOutput, true, grdToWho); // gradient to Who
            Matrix<ElemType>::MultiplyAndAdd(grdBeforeGo, false, obs, true, grdToWxo); // gradient to Wxo
            tmpMat.AssignInnerProductOf(grdBeforeGo, state, false);
            grdToWco += tmpMat;                    // to Wco
            for (size_t i = 0; i < grdBeforeGo.GetNumCols(); i++)
            {
                grdTobo += grdBeforeGo.ColumnSlice(i, 1);  // gradient to bo
            }

            grdToGo.AssignElementProductOf(outGrd, go);  // error to tanh
            GradientOfTanh(tanhState, grdToGo, grdToCell, tmpMat); // error to memory cell
            grdToCell += stateError; // add error to memory cell from t+1
#ifdef DEBUG_DECODER
            fprintf(stderr, "previous state[0] = %.4e norm = %.4e\n", prevState(0, 0), prevState.FrobeniusNorm());
            fprintf(stderr, "state error = %.4e\n", grdToCell(0, 0));
            fprintf(stderr, "state error norm = %.4e\n", grdToCell.FrobeniusNorm());
#endif
            // error backpropagate to memory cells
            grdToPrevState.AssignElementProductOf(gf, grdToCell);  // error to previous memory cell
            // be careful, need to double check if errors are missing

            Matrix<ElemType> grdBeforeForget(tmpMat.GetDeviceId());
            tmpMat.AssignElementProductOf(prevState, grdToCell);  // error to f_t
            gradientOfSigmoid.AssignSigmoidDerivativeOf(gf);
            grdBeforeForget.AssignElementProductOf(gradientOfSigmoid, tmpMat); // error before forget gate
#ifdef DEBUG_DECODER
            fprintf(stderr, "forget gate error = %.4e\n", grdBeforeForget(0, 0));
#endif

            Matrix<ElemType>::MultiplyAndAdd(Whf, true, grdBeforeForget, false, grdToPrevOutput);  // error to previous output
            tmpMat = grdBeforeForget;
            tmpMat.ColumnElementMultiplyWith(Wcf);
            grdToPrevState += tmpMat;                                                            // error to previous state

            Matrix<ElemType>::MultiplyAndAdd(Wxf, true, grdBeforeForget, false, grdToObs);  // error to observation

            Matrix<ElemType>::MultiplyAndAdd(grdBeforeForget, false, prevOutput, true, grdToWhf); // gradient to Whf
            tmpMat.AssignInnerProductOf(grdBeforeForget, prevState, false);
            grdToWcf += tmpMat;                                                             // gradient to Wcf

            Matrix<ElemType>::MultiplyAndAdd(grdBeforeForget, false, obs, true, grdToWxf); // gradient to Wxf
            for (size_t i = 0; i < grdBeforeForget.GetNumCols(); i++)
                grdTobf += grdBeforeForget.ColumnSlice(i, 1);                                                    // gradient to bf

            // error backpropagate to input gate
            tmpMat.AssignElementProductOf(tanhBeforeApplyingInputGating, grdToCell);
            gradientOfSigmoid.AssignSigmoidDerivativeOf(gi);
            grdBeforeInputGate.AssignElementProductOf(gradientOfSigmoid, tmpMat); // error before input gate
#ifdef DEBUG_DECODER
            fprintf(stderr, "input gate error = %.4e\n", grdBeforeInputGate(0, 0));
#endif

            Matrix<ElemType>::MultiplyAndAdd(Whi, true, grdBeforeInputGate, false, grdToPrevOutput);  // error to previous output
            tmpMat = grdBeforeInputGate;
            tmpMat.ColumnElementMultiplyWith(Wci);
            grdToPrevState += tmpMat;                                                            // error to previous state

#ifdef DEBUG_DECODER
            fprintf(stderr, "to previous state error = %.4e\n", grdToPrevState(0, 0));
            fprintf(stderr, "to previous state error norm = %.4e\n", grdToPrevState.FrobeniusNorm());
#endif
            Matrix<ElemType>::MultiplyAndAdd(Wxi, true, grdBeforeInputGate, false, grdToObs);  // error to observation

            Matrix<ElemType>::MultiplyAndAdd(grdBeforeInputGate, false, prevOutput, true, grdToWhi); // gradient to Whi
            tmpMat.AssignInnerProductOf(grdBeforeInputGate, prevState, false);
            grdToWci += tmpMat;                                                             // gradient to Wci
            Matrix<ElemType>::MultiplyAndAdd(grdBeforeInputGate, false, obs, true, grdToWxi); // gradient to Wxi
            for (size_t i = 0; i < grdBeforeInputGate.GetNumCols(); i++)
                grdTobi += grdBeforeInputGate.ColumnSlice(i, 1);                                                  // gradient to bi

            // error backpropagate to inputs
            Matrix<ElemType> grdTmp2(tmpMat.GetDeviceId());
            Matrix<ElemType> grdBeforeTanhInputGate(tmpMat.GetDeviceId());
            grdTmp2.AssignElementProductOf(gi, grdToCell);
            grdBeforeTanhInputGate.Resize(tanhBeforeApplyingInputGating.GetNumRows(), tanhBeforeApplyingInputGating.GetNumCols());
            GradientOfTanh(tanhBeforeApplyingInputGating, grdTmp2, grdBeforeTanhInputGate, tmpMat); // error to memory cell
            Matrix<ElemType>::MultiplyAndAdd(Wxc, true, grdBeforeTanhInputGate, false, grdToObs);  // error to observation
#ifdef DEBUG_DECODER
            fprintf(stderr, "to observation error = %.4e\n", grdToObs(0, 0));
#endif

            Matrix<ElemType>::MultiplyAndAdd(Whc, true, grdBeforeTanhInputGate, false, grdToPrevOutput);  // error to previous output
            Matrix<ElemType>::MultiplyAndAdd(grdBeforeTanhInputGate, false, obs, true, grdToWxc); // gradient to Wxc

            Matrix<ElemType>::MultiplyAndAdd(grdBeforeTanhInputGate, false, prevOutput, true, grdToWhc); // gradient to Whc
            for (size_t i = 0; i < grdBeforeTanhInputGate.GetNumCols(); i++)
                grdTobc += grdBeforeTanhInputGate.ColumnSlice(i, 1);                                                    // gradient to bc

        }

        /**
        get the segmentation information, SENTENECE_BEGIN, SEQUENCE_MIDDLE, NO_INPUT 
        for time at t and stream of streamid
        */
        int GetSegInfo(size_t t, size_t streamid)
        {
            if (streamid >= m_samplesInRecurrentStep)
                LogicError("GetSegInfo: stream id %d is larger than the number of streams %d", streamid, m_samplesInRecurrentStep);

            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            if (t >= nT)
                LogicError("GetSegInfo: time %d times is larger than the total number of observations %d", t, nT);

            int utt_t = (int)t / m_samplesInRecurrentStep;
            Matrix<ElemType> thisCol = m_sentenceSeg->ColumnSlice(utt_t, 1);
            thisCol.Reshape(1, m_samplesInRecurrentStep);
            return (int) thisCol.ColumnSlice(streamid, 1).Get00Element();
        }

        /**
        save the last hidden layer activity and output
        */
        void SaveLastStateActity()
        {
            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            size_t outputDim = Inputs(1)->FunctionValues().GetNumRows();
            
            // save the hidden activities and output for the next minibatch
            mLastOutput.Resize(outputDim, m_samplesInRecurrentStep);
            mLastState.Resize(outputDim, m_samplesInRecurrentStep);

            for (size_t i = 0; i < m_samplesInRecurrentStep; i++)
            {
                for (int t = nT - m_samplesInRecurrentStep + i; t >= 0; t -= m_samplesInRecurrentStep)
                {
                    if (GetSegInfo(t, i) == SEQUENCE_MIDDLE)
                    {
                        mLastOutput.ColumnSlice(i, 1).SetValue(FunctionValues().ColumnSlice(t, 1));
                        mLastState.ColumnSlice(i, 1).SetValue(m_State.ColumnSlice(t, 1));
                        break;
                    }
                }
            }
        }

        virtual void EvaluateThisNode()
        {
            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            size_t outputDim = Inputs(1)->FunctionValues().GetNumRows();

            {
                FunctionValues().Resize(outputDim, nT);
                FunctionValues().SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                m_State.Resize(outputDim, nT);
                m_State.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                m_Gi.Resize(outputDim, nT);
                m_Gi.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                m_Gf.Resize(outputDim, nT);
                m_Gf.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                m_Go.Resize(outputDim, nT);
                m_Go.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                tanhState.Resize(outputDim, nT);
                tanhState.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
                tanhObs.Resize(outputDim, nT);
                tanhObs.SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 

                if (m_PastState.IsEmpty() || m_PastState.GetNumCols() != m_samplesInRecurrentStep)
                {
                    m_PastState.Resize(outputDim, m_samplesInRecurrentStep);
                    m_PastState.SetValue(m_DefaultState);
                }
                if (m_PastOutput.IsEmpty() || m_PastOutput.GetNumCols() != m_samplesInRecurrentStep)
                {
                    m_PastOutput.Resize(outputDim, m_samplesInRecurrentStep);
                }

#ifdef DEBUG_DECODER
                if (m_PastOutput.IsEmpty() == false)
                    fprintf(stderr, "LSTM node %ls past output norm = %.8e\n", this->NodeName().c_str(), m_PastOutput.FrobeniusNorm());
                if (m_PastState.IsEmpty() == false)
                    fprintf(stderr, "LSTM node %ls past state norm = %.8e\n", this->NodeName().c_str(), m_PastState.FrobeniusNorm());
#endif

                for (size_t timeIdxInSeq = 0; timeIdxInSeq < nT; timeIdxInSeq += m_samplesInRecurrentStep)
                {
                    FrameRange frameRange(timeIdxInSeq);
                    Matrix<ElemType> sliceObs = Inputs(0)->FunctionValues().ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceOutput = FunctionValues().ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceState = m_State.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);

                    Matrix<ElemType> sliceGi = m_Gi.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceGf = m_Gf.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceGo = m_Go.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);

                    Matrix<ElemType> sliceTanhState = tanhState.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);
                    Matrix<ElemType> sliceTanhInput =
                        tanhObs.ColumnSlice(frameRange.t(), m_samplesInRecurrentStep);

                    PrepareHistory(timeIdxInSeq, mSlicePrevOutput, mSlicePrevState, FunctionValues(), m_State, m_PastOutput, m_PastState, m_samplesInRecurrentStep, m_DefaultState, m_sentenceSeg);

                        EvaluateThisNodeS(Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), Inputs(4)->FunctionValues(),
                            sliceObs, mSlicePrevOutput, mSlicePrevState, sliceOutput, sliceState, sliceGi, sliceGf, sliceGo, sliceTanhState, sliceTanhInput, m_tempMatrix);
                    }

                // save the hidden activities and output for the next minibatch
                SaveLastStateActity();

#ifdef DEBUG_DECODER
                if (mLastOutput.IsEmpty() == false)
                    fprintf(stderr, "LSTM node %ls last output norm = %.8e\n", this->NodeName().c_str(), mLastOutput.FrobeniusNorm());
                if (mLastState.IsEmpty() == false)
                    fprintf(stderr, "LSTM node %ls last state norm = %.8e\n", this->NodeName().c_str(), mLastState.FrobeniusNorm());
#endif

#ifdef DEBUG_DECODER
                ElemType tmpnorm = FunctionValues().FrobeniusNorm();
                if (ISCLOSE(tmpnorm, 0.834251, 0.002))
                    fprintf(stderr, "check!");
                fprintf(stderr, "LSTM function norm = %.8e\n", tmpnorm);
                for (size_t i = 0; i < 5; i++)
                    fprintf(stderr, "LSTM input[%d] norm = %.8e ", i, Inputs(i)->FunctionValues().FrobeniusNorm());
                fprintf(stderr, "\n");
#endif

                m_GradientComputed = false;
            }
        }

        /**
        Prepare history for LSTMnode

        This function returns state and output from the previous time instance. For recurrent network, the initial state needs to be set in the case of sentence begining, which is carried over from sentenceBegin. In case of sentence begining, the state activity is set to an initial value. The sentenceBegin has element of SEQUENCE_START, SEQUENCE_MIDDLE and NO_INPUT, which are 0, 1, and -1, respectively. 
        To compute the initial value, we use
        prevState = sentenceBegin * delayedActivation + ~sentenceBegin * initialStateValue
        and ~sentenceBegin is computed as -1*(sentenceBegin - 1), assuming that sentenceBegin is either 0 or 1. For example, when sentenceBegin == 1, ~sentenceBegin == 0. 
        The previous-time output doesn't have initial value, so it is computed as 
        prevOutput = sentenceBegin * pastOutput

        */
        // prepare prevstate and prevoutput
        static void WINAPI PrepareHistory(
            size_t timeIdxInSeq,
            Matrix<ElemType> & slicePrevOutput,
            Matrix<ElemType> & slicePrevState,
            const Matrix<ElemType> & output,
            const Matrix<ElemType> & state,
            const Matrix<ElemType> & pastOutput,
            const Matrix<ElemType> & pastState,
            size_t nsamples, const ElemType & initStateValue, Matrix<ElemType>* sentenceBegin)
        {
            size_t nRow = pastOutput.GetNumRows();
            size_t nStream = sentenceBegin->GetNumRows();

            assert(nStream == nsamples);

            int utt_t = (int)floor(timeIdxInSeq / nsamples);
            if (slicePrevOutput.IsEmpty() || slicePrevOutput.GetNumRows() != nRow || slicePrevOutput.GetNumCols() != nsamples)
                slicePrevOutput.Resize(nRow, nsamples);
            if (slicePrevState.IsEmpty() || slicePrevState.GetNumRows() != nRow || slicePrevState.GetNumCols() != nsamples)
                slicePrevState.Resize(nRow, nsamples);

            if (sentenceBegin->GetNumRows() != nsamples)
                LogicError("Number of rows should be the same as the number of data streams");

            Matrix<ElemType> colBegin(sentenceBegin->GetDeviceId());
            colBegin.SetValue(sentenceBegin->ColumnSlice(utt_t, 1));
            Matrix<ElemType> colSeg(colBegin.GetDeviceId()); 
            colSeg.Resize(nStream, nStream);
            // will reset to 0 if sentence begining at a posiiton is 0
            // will keep the output if it is not the sentence begining
            colBegin.InplaceTruncateBottom(SEQUENCE_START);
            colBegin.InplaceTruncateTop(SEQUENCE_MIDDLE);
            colSeg.SetDiagonalValue(colBegin);

            Matrix<ElemType> newPrevOutput(colBegin.GetDeviceId());
            Matrix<ElemType> newPrevState(colBegin.GetDeviceId());
            if (utt_t == 0)
            {
                // this is the begining of this minibatch
                Matrix<ElemType>::Multiply(pastOutput.ColumnSlice(0, nsamples), false, colSeg, false, newPrevOutput);
                Matrix<ElemType>::Multiply(pastState.ColumnSlice(0, nsamples), false, colSeg, false, newPrevState);

            }
            else
            {
                // this is in the minibatch
                FrameRange frameRange(timeIdxInSeq);
                Matrix<ElemType>::Multiply(output.ColumnSlice(frameRange.t() - nsamples, nsamples), false, colSeg, false, newPrevOutput);
                Matrix<ElemType>::Multiply(state.ColumnSlice(frameRange.t() - nsamples, nsamples), false, colSeg, false, newPrevState);
            }

            ComputationNode<ElemType>::SetToInitStateValueForResetSeg(sentenceBegin->ColumnSlice(utt_t, 1), nStream, initStateValue, newPrevState);

            slicePrevOutput.ColumnSlice(0, nsamples).SetValue(newPrevOutput);
            slicePrevState.ColumnSlice(0, nsamples).SetValue(newPrevState);
        }

        // prepare prevstate and prevoutput
        void PrepareThisErrorsBeforeBackProp(
            size_t timeIdxInSeq,
            size_t nT, // number of columns
            Matrix<ElemType> & error,
            Matrix<ElemType> & stateError,
            const Matrix<ElemType>& grdToPrevOutput,
            const Matrix<ElemType>& grdToPrevState,
            const Matrix<ElemType>& obs_error_from_future_minibatch,
            const Matrix<ElemType>& state_error_from_future_minibatch,
            size_t nsamples, Matrix<ElemType>* sentenceBegin)
        {
            int utt_t = (int)floor(timeIdxInSeq / nsamples);
            int total_utt_t = (int)floor(nT / nsamples);

            error += grdToPrevOutput;
            stateError = grdToPrevState;

            if (m_use_errors_from_future_minibatch)
            {
                for (size_t utt_id = 0; utt_id < nsamples; utt_id++)
                {
                    // if uses errors from future minibatch
                    if ((GetSegInfo(timeIdxInSeq, utt_id) == SEQUENCE_MIDDLE && utt_t == total_utt_t - 1) // last time 
                        || (utt_t < total_utt_t - 1 && GetSegInfo(timeIdxInSeq, utt_id) == SEQUENCE_MIDDLE && GetSegInfo(timeIdxInSeq + nsamples, utt_id) == NO_INPUT) // future observation is no observation
                        )
                    {
                        error.ColumnSlice(utt_id, 1) += obs_error_from_future_minibatch.ColumnSlice(utt_id, 1);
                        stateError.ColumnSlice(utt_id, 1) += state_error_from_future_minibatch.ColumnSlice(utt_id, 1);
                    }
                }
            }


            Matrix<ElemType> colBegin(sentenceBegin->GetDeviceId());
            colBegin.SetValue(sentenceBegin->ColumnSlice(utt_t, 1));
            colBegin.InplaceTruncateBottom(NO_INPUT);
            colBegin.InplaceTruncateTop(SEQUENCE_START);
            colBegin += fabs((ElemType)NO_INPUT); // raise this so that -1 -> 0 and therefore 
            Matrix<ElemType> colSeg(colBegin.GetDeviceId());
            colSeg.Resize(nsamples, nsamples);
            colSeg.SetDiagonalValue(colBegin);

            // times the errors with the mask
            Matrix<ElemType> newOutputError(colBegin.GetDeviceId());
            Matrix<ElemType> newStateError(colBegin.GetDeviceId());

            Matrix<ElemType>::Multiply(error, false, colSeg, false, newOutputError);
            Matrix<ElemType>::Multiply(stateError, false, colSeg, false, newStateError);
            
            error.ColumnSlice(0, nsamples).SetValue(newOutputError);
            stateError.ColumnSlice(0, nsamples).SetValue(newStateError);
        }

        // prepare prevstate and prevoutput
        static void WINAPI PrepareErrors(
            size_t timeIdxInSeq,
            Matrix<ElemType> & errors,
            Matrix<ElemType> & stateError,
            size_t nsamples, Matrix<ElemType>* sentenceBegin)
        {
            int utt_t = (int)floor(timeIdxInSeq / nsamples);
            Matrix<ElemType> colBegin(sentenceBegin->GetDeviceId());
            colBegin.SetValue(sentenceBegin->ColumnSlice(utt_t, 1));
            // will reset to 0 if sentence begining at a posiiton is 0
            // will keep the output if it is not the sentence begining
            colBegin.InplaceTruncateBottom(SEQUENCE_START);
            colBegin.InplaceTruncateTop(SEQUENCE_MIDDLE);

            Matrix<ElemType> colSeg(colBegin.GetDeviceId());
            colSeg.Resize(nsamples, nsamples);
            colSeg.SetDiagonalValue(colBegin);

            // times the errors with the mask
            Matrix<ElemType> newOutputError(colBegin.GetDeviceId());
            Matrix<ElemType> newStateError(colBegin.GetDeviceId());

            Matrix<ElemType>::Multiply(errors, false, colSeg, false, newOutputError);
            Matrix<ElemType>::Multiply(stateError, false, colSeg, false, newStateError);

            errors.ColumnSlice(0, nsamples).SetValue(newOutputError);
            stateError.ColumnSlice(0, nsamples).SetValue(newStateError);
        }

        static void WINAPI EvaluateThisNodeS(
            const Matrix<ElemType>& mInputGate,
            const Matrix<ElemType> &mForgetGate, const Matrix<ElemType> &mOutputGate,
            const Matrix<ElemType> &mCellWgt,
            const Matrix<ElemType> &obs,
            const Matrix<ElemType>& prevOutput,
            const Matrix<ElemType>& prevState,
            Matrix<ElemType> &output,
            Matrix<ElemType> &state,
            Matrix<ElemType> &gi,
            Matrix<ElemType> &gf,
            Matrix<ElemType> &go,
            Matrix<ElemType> &tanhState,
            Matrix<ElemType> &tanhObs,
            Matrix<ElemType> &tmp)
        {
            int inputDim = obs.GetNumRows();
            int outputDim = mOutputGate.GetNumRows();

            // for input gate
            Matrix<ElemType>::Multiply(mInputGate.ColumnSlice(1, inputDim), false, obs, false, gi);
            Matrix<ElemType>::MultiplyAndAdd(mInputGate.ColumnSlice(1 + inputDim, outputDim), false, prevOutput, false, gi);
            gi += mInputGate.ColumnSlice(0, 1);
            tmp = prevState;
            tmp.ColumnElementMultiplyWith(mInputGate.ColumnSlice(1 + inputDim + outputDim, 1));
            gi += tmp;
            gi.AssignSigmoidOf(gi);

            // for forget gate
            Matrix<ElemType>::Multiply(mForgetGate.ColumnSlice(1, inputDim), false, obs, false, gf);
            Matrix<ElemType>::MultiplyAndAdd(mForgetGate.ColumnSlice(1 + inputDim, outputDim), false, prevOutput, false, gf);
            gf += mForgetGate.ColumnSlice(0, 1);
            tmp = prevState;
            tmp.ColumnElementMultiplyWith(mForgetGate.ColumnSlice(1 + inputDim + outputDim, 1));
            gf += tmp;
            gf.AssignSigmoidOf(gf);

            // for cell state
            Matrix<ElemType>::Multiply(mCellWgt.ColumnSlice(1, inputDim), false, obs, false, state);
            Matrix<ElemType>::MultiplyAndAdd(mCellWgt.ColumnSlice(1 + inputDim, outputDim), false, prevOutput, false, state);
            state += mCellWgt.ColumnSlice(0, 1);
#ifdef DEBUG_DECODER
//            fprintf(stderr, "W_xc norm = %.8e\n", mCellWgt.ColumnSlice(1, inputDim).FrobeniusNorm());
//            fprintf(stderr, "W_hc norm = %.8e\n", mCellWgt.ColumnSlice(1 + inputDim, outputDim).FrobeniusNorm());
//            fprintf(stderr, "b_c norm = %.8e\n", mCellWgt.ColumnSlice(0, 1).FrobeniusNorm());
#endif
            tanhObs.AssignTanhOf(state);
            state.AssignElementProductOf(gi, tanhObs);
            state.AddElementProductOf(gf, prevState);

            // for output gate
            Matrix<ElemType>::Multiply(mOutputGate.ColumnSlice(1, inputDim), false, obs, false, go);
            Matrix<ElemType>::MultiplyAndAdd(mOutputGate.ColumnSlice(1 + inputDim, outputDim), false, prevOutput, false, go);
            go += mOutputGate.ColumnSlice(0, 1);
            tmp = state;
            tmp.ColumnElementMultiplyWith(mOutputGate.ColumnSlice(1 + inputDim + outputDim, 1));
            go += tmp;
            go.AssignSigmoidOf(go);

            // to return output
            tanhState.AssignTanhOf(state);
            output.AssignElementProductOf(go, tanhState);
        }


        // input(0) : child with dimension [inputdim x T]
        // input(1) : input gate [outputdim x [inputdim + outputdim + 2]] bi, Wxi, Whi, Wci
        // input(2) : forget gate [outputdim x [inputdim + outputdim + 2]] for bf, Wxf, Whf, Wcf
        // input(3) : output gate [outputdim x [inputdim + outputdim + 2]] for bo, Wxo, Who, and Wco
        // input(4) : memory cell weight [outputdim x [inputdim + outputdim + 1]] for bc, Wxc, and Whc 
        // output : dimension [outputdim x T]
        virtual void Validate()
        {
            PrintSelfBeforeValidation();

            if (m_children.size() != 5)
                throw std::logic_error("LSTMNode requires four inputs.");

            InferImageDimsFromInputs();

            if (Inputs(0)->FunctionValues().GetMatrixType() == SPARSE)
                LogicError("LSTMNode: input to LSTM has to be dense matrix. Consider adding a project layer using lookuptable before LSTM node. ");

            if (Inputs(1)->OperationName() != LearnableParameter<ElemType>::TypeName() ||
                Inputs(2)->OperationName() != LearnableParameter<ElemType>::TypeName() ||
                Inputs(3)->OperationName() != LearnableParameter<ElemType>::TypeName() ||
                Inputs(4)->OperationName() != LearnableParameter<ElemType>::TypeName())
                throw std::logic_error("LSTM validation: need to have learnable parameters ");

            if (Inputs(0)->FunctionValues().HasNoElements())
                throw std::logic_error("LSTM validation: input size is zero!");

            if (Inputs(1)->FunctionValues().HasNoElements() ||
                Inputs(2)->FunctionValues().HasNoElements() ||
                Inputs(3)->FunctionValues().HasNoElements() ||
                Inputs(4)->FunctionValues().HasNoElements())
                throw std::logic_error("LSTM validation : parameter size is zero!");


            size_t nindim = Inputs(0)->FunctionValues().GetNumRows();
            size_t noutdim = Inputs(1)->FunctionValues().GetNumRows();
            size_t nT = Inputs(0)->FunctionValues().GetNumCols();
            size_t nCol = nindim + noutdim + 2;
            if (Inputs(1)->FunctionValues().GetNumCols() != nCol)
            {
                throw std::logic_error("LSTM validation : dimension mismatched between child and inputGate");
            }
            if (Inputs(2)->FunctionValues().GetNumCols() != nCol)
            {
                throw std::logic_error("LSTM validation : dimension mismatched between child and forgetGate");
            }
            if (Inputs(3)->FunctionValues().GetNumCols() != nCol)
            {
                throw std::logic_error("LSTM validation : dimension mismatched between child and outputGate");
            }

            if (noutdim != Inputs(2)->FunctionValues().GetNumRows() ||
                noutdim != Inputs(3)->FunctionValues().GetNumRows() ||
                noutdim != Inputs(4)->FunctionValues().GetNumRows())
            {
                throw std::logic_error("LSTM validation: output dimension mismatched!");
            }

            FunctionValues().Resize(noutdim, nT);
            FunctionValues().SetValue(NAN);  // set to this extrem value so, if anything wrong in later procedure, problems can be easily spotted. 
        }

        bool UnitTest()
        {
            {
                size_t nT = 3;
                size_t nInput = 2;
                size_t nHidden = 3;
                size_t nOutput = 3;

                // backup 
                Matrix<ElemType> f0(m_deviceId), f1(m_deviceId), f2(m_deviceId), f3(m_deviceId), f4(m_deviceId), func(m_deviceId), f5(m_deviceId);
                Matrix<ElemType> target(m_deviceId);
                Matrix<ElemType> giWeight, ghWeight, goWeight;
                ElemType initStateValue = m_DefaultState;
                Matrix<ElemType> boundary(m_deviceId);
                boundary.Resize(1, nT);
                boundary.SetValue(SEQUENCE_MIDDLE);
                boundary.ColumnSlice(0, 1).SetValue(SEQUENCE_START);

                vector<MinibatchPackingFlag> minibatchPackingFlag;
                minibatchPackingFlag.resize(nT);
                std::fill(minibatchPackingFlag.begin(), minibatchPackingFlag.end(), MinibatchPackingFlag::None);
                minibatchPackingFlag[1] = MinibatchPackingFlag::SequenceStart;
                ComputationNode<ElemType>::ResetBound(&boundary, &minibatchPackingFlag);

                f0 = Inputs(0)->FunctionValues();
                f1 = Inputs(1)->FunctionValues();
                f2 = Inputs(2)->FunctionValues();
                f3 = Inputs(3)->FunctionValues();
                f4 = Inputs(4)->FunctionValues();
                func = FunctionValues();

                target.Resize(nOutput, nT);
                for (size_t i = 0; i < nT; i++)
                    target(0, i) = 1;

                Inputs(0)->FunctionValues().Resize(nInput, nT);
                Inputs(0)->FunctionValues().SetValue(ConstOnes(nInput, nT, m_deviceId));
                Inputs(0)->FunctionValues().SetValue((ElemType)0.1);
                Inputs(1)->FunctionValues().Resize(nHidden, nInput + nOutput + 2);
                Inputs(1)->FunctionValues().SetValue((ElemType)0.1);
                Inputs(2)->FunctionValues().Resize(nHidden, nInput + nHidden + 2);
                Inputs(2)->FunctionValues().SetValue((ElemType)0.1);
                Inputs(3)->FunctionValues().Resize(nOutput, nInput + nHidden + 2);
                Inputs(3)->FunctionValues().SetValue((ElemType)0.1);
                Inputs(4)->FunctionValues().Resize(nOutput, nHidden + nInput + 1);
                Inputs(4)->FunctionValues().SetValue((ElemType)0.1);
                FunctionValues().Resize(nOutput, nT);

                m_DefaultState = 0.0;
                EvaluateThisNode();

                // check with expected values
                if (!ISCLOSE(FunctionValues()(0, 0), 0.0335975, EPSILON) ||
                    !ISCLOSE(FunctionValues()(0, 1), 0.05485132, EPSILON) ||
                    !ISCLOSE(FunctionValues()(0, 2), 0.06838435, EPSILON) ||
                    !(FunctionValues()(0, 0) == FunctionValues()(1, 0)))
                    throw("LSTMNode forward computation error");

                
                    FunctionValues().TransferToDeviceIfNotThere( m_deviceId, true);

                GradientValues().Resize(nOutput, nT);
                GradientValues().SetValue(1.0);
                for (size_t i = 0; i < 5; i++)
                {
                    Inputs(i)->GradientValues().Resize(Inputs(i)->FunctionValues().GetNumRows(), Inputs(i)->FunctionValues().GetNumCols());
                    Inputs(i)->GradientValues().SetValue(0);
                }
                for (size_t i = 0; i < 5; i++)
                    ComputeInputPartial(i);

                // check with expected values
                if (!ISCLOSE(Inputs(1)->GradientValues()(0, 0), 0.07843818, EPSILON) // bi
                    || !ISCLOSE(Inputs(1)->GradientValues()(0, 1), 0.00784382, EPSILON)  // Wxi
                    || !ISCLOSE(Inputs(1)->GradientValues()(0, 3), 0.00192997, EPSILON)  // Whi
                    || !ISCLOSE(Inputs(1)->GradientValues()(0, 6), 0.00362767, EPSILON)  // Wci
                    )
                    throw("LSTMNode gradient error on input gates");
                if (!ISCLOSE(Inputs(2)->GradientValues()(0, 0), 0.02738655, EPSILON)  // bf
                    || !ISCLOSE(Inputs(2)->GradientValues()(0, 1), 0.00273866, EPSILON)  // Wxf
                    || !ISCLOSE(Inputs(2)->GradientValues()(0, 3), 0.00120922, EPSILON)  // Whf
                    || !ISCLOSE(Inputs(2)->GradientValues()(0, 6), 0.00227184, EPSILON)  // Wcf
                    )
                    throw("LSTMNode gradient error on forget gates");
                if (!ISCLOSE(Inputs(3)->GradientValues()(0, 0), 0.07801557, EPSILON)  // bo
                    || !ISCLOSE(Inputs(3)->GradientValues()(0, 1), 0.00780156, EPSILON)  // Wxo
                    || !ISCLOSE(Inputs(3)->GradientValues()(0, 3), 0.00268089, EPSILON)  // Who
                    || !ISCLOSE(Inputs(3)->GradientValues()(0, 6), 0.00809852, EPSILON)  // Wco
                    )
                    throw("LSTMNode gradient error on output gates");
                if (!ISCLOSE(Inputs(4)->GradientValues()(0, 0), 1.3075038, EPSILON)  // bc
                    || !ISCLOSE(Inputs(4)->GradientValues()(0, 1), 0.13075038, EPSILON)  // Wxc
                    || !ISCLOSE(Inputs(4)->GradientValues()(0, 3), 0.03080355, EPSILON)  // Whc
                    )
                    throw("LSTMNode gradient error on memory cells");

                for (size_t i = 0; i < 5; i++)
                {
                    
                        Inputs(i)->GradientValues().TransferToDeviceIfNotThere( m_deviceId, true);
                }
                m_DefaultState = initStateValue;
            }

            fprintf(stderr, "LSTMNode unit test passed!\n");
            return true;
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(1, false);
        }

        // input(0) : child with dimension [inputdim x T]
        // input(1) : input gate [outputdim x [inputdim + outputdim + 2]] bi, Wxi, Whi, Wci
        // input(2) : forget gate [outputdim x [inputdim + outputdim + 2]] for bf, Wxf, Whf, Wcf
        // input(3) : output gate [outputdim x [inputdim + outputdim + 2]] for bo, Wxo, Who, and Wco
        // input(4) : memory cell weight [outputdim x [inputdim + outputdim + 1]] for bc, Wxc, and Whc 
        // output : dimension [outputdim x T]
        virtual void AttachInputs(const ComputationNodePtr obs, const ComputationNodePtr inputGate, const ComputationNodePtr forgetGate, const ComputationNodePtr outputGate, const ComputationNodePtr memoryCellWgt)
        {
            m_children.resize(5);
            m_children[0] = obs;
            m_children[1] = inputGate;
            m_children[2] = forgetGate;
            m_children[3] = outputGate;
            m_children[4] = memoryCellWgt;
        }

        virtual void MoveMatricesToDevice(const short deviceId)
        {
            ComputationNode<ElemType>::MoveMatricesToDevice(deviceId);
            m_functionValues.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true, m_functionValues.HasNoElements());
            m_gradientValues.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId, true, m_gradientValues.HasNoElements());
            grdToObs.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdToInputGate.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdToForgetGate.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdToOutputGate.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdToCellWgt.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_State.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_PastState.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_PastOutput.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_Gi.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_Gf.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_Go.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            tanhState.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            tanhObs.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_tempMatrix.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            mSlicePrevState.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            mSlicePrevOutput.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdBeforeInputGate.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdBeforeForget.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdBeforeGo.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdToCell.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            grdBeforeTanhInputGate.TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        virtual void DumpNodeInfo(const bool printValues, File& fstream) const
        {
            ComputationNode<ElemType>::DumpNodeInfo(printValues, fstream);

            fstream << L"Input[Width:" << m_inputDim << L"]  \n" ; 
            fstream << L"Hidden[Width:" << m_outputDim << L"]    Output[Width:" << m_outputDim << L"]  \n";
        }


    public:

        bool GetHistory(Matrix<ElemType>& hist, bool bLastTime)
        {
            size_t tRow = m_PastOutput.GetNumRows();
            size_t tCol = m_PastOutput.GetNumCols();
            size_t rCol = m_PastState.GetNumCols();

            DEVICEID_TYPE device = hist.GetDeviceId();
            hist.TransferFromDeviceToDevice(device, m_deviceId, true);
            hist.Resize(tRow, tCol + rCol);

            if (bLastTime)
            {
                hist.ColumnSlice(0, tCol).SetValue(mLastOutput);
                hist.ColumnSlice(tCol, rCol).SetValue(mLastState);
            }
            else{
                hist.ColumnSlice(0, tCol).SetValue(m_PastOutput);
                hist.ColumnSlice(tCol, rCol).SetValue(m_PastState);
            }

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
            return true;
        }

        void SetHistory(const Matrix<ElemType>& hist)
        {
            size_t tRow = hist.GetNumRows();
            size_t tCol = hist.GetNumCols();
            size_t eCols = tCol / 2;

            DEVICEID_TYPE device = hist.GetDeviceId();
            hist.TransferFromDeviceToDevice(device, m_deviceId, true);

            m_PastOutput.Resize(tRow, eCols);
            m_PastState.Resize(tRow, eCols);
            m_PastOutput.SetValue(hist.ColumnSlice(0, eCols));
            m_PastState.SetValue(hist.ColumnSlice(eCols, eCols));

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
        }

        virtual void GetErrorsToPreviousMinibatch(Matrix<ElemType>& hist)
        {
            size_t tRow = m_obs_error_from_future_minibatch.GetNumRows();
            size_t tCol = m_obs_error_from_future_minibatch.GetNumCols();
            size_t rCol = m_state_error_from_future_minibatch.GetNumCols();

            DEVICEID_TYPE device = hist.GetDeviceId();

            hist.TransferFromDeviceToDevice(device, m_deviceId, true);
            hist.Resize(tRow, tCol + rCol);

            hist.ColumnSlice(0, tCol).SetValue(m_obs_error_from_future_minibatch);
            hist.ColumnSlice(tCol, rCol).SetValue(m_state_error_from_future_minibatch);

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
        }

        virtual void SetErrorsFromFutureMinibatch(Matrix<ElemType>& hist)
        {
            size_t tCol = hist.GetNumCols();
            size_t rCol = tCol / 2;

            DEVICEID_TYPE device = hist.GetDeviceId();

            hist.TransferFromDeviceToDevice(device, m_deviceId, true);

            m_obs_error_from_future_minibatch.SetValue(hist.ColumnSlice(0, rCol));
            m_state_error_from_future_minibatch.SetValue(hist.ColumnSlice(rCol, rCol));

            m_use_errors_from_future_minibatch = true;

            hist.TransferFromDeviceToDevice(m_deviceId, device, true);
        }

    protected:
        virtual bool UseCustomizedMultiSeqHandling() { return true; }

    protected:
        size_t m_inputDim;
        size_t m_outputDim;

        Matrix<ElemType> m_State;  // hidden state activity
        Matrix<ElemType> m_PastState; // state activity in the previous minibatch
        Matrix<ElemType> m_PastOutput; // output in the previou minibatch 

        Matrix<ElemType> mLastState; // last state activity 
        Matrix<ElemType> mLastOutput; // last output 

        Matrix<ElemType> m_Gi;     // input gate activity
        Matrix<ElemType> m_Gf;     // forget gate activity
        Matrix<ElemType> m_Go;     // output gate activity

        Matrix<ElemType> grdToObs, grdToInputGate, grdToForgetGate, grdToOutputGate, grdToCellWgt;
        Matrix<ElemType> tanhState, tanhObs;

        Matrix<ElemType> m_tempMatrix; // temp matrix for speed-up

        bool     m_GradientComputed; // true if LSTM node has computed gradients, set to false if forward computation is just finished 

        Matrix<ElemType> mSlicePrevOutput, mSlicePrevState;

        Matrix<ElemType> grdBeforeInputGate, grdBeforeForget, grdBeforeGo, grdToCell, grdBeforeTanhInputGate;

    public:
        // errors from future minibatch
        Matrix<ElemType> m_obs_error_from_future_minibatch;
        Matrix<ElemType> m_state_error_from_future_minibatch;
        bool m_use_errors_from_future_minibatch;

        ElemType m_DefaultState;

    };

    template class LSTMNode<float>;
    template class LSTMNode<double>;

}}}
