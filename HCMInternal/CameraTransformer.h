#pragma once
#include "pch.h"
#include "FreeCameraData.h"
#include "NullSmoother.h"
#include "LinearSmoother.h"
#include "MomentumSmoother.h"
#include "IUpdateCameraTransform.h"

using namespace SettingsEnums;

class RotationTransformer
{
private:
	ScopedCallback< eventpp::CallbackList<void(FreeCameraInterpolationTypesEnum&)>> currentInterpolationTypeChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentLinearInterpolationFactorChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentDriftChangedCallback;

	void applyRotationTransform(FreeCameraData& freeCameraData, float frameDelta)
	{

		// interpolate euler angles
		pCurrentRotationSmoother->smooth(currentEulerYaw, targetEulerYaw);
		pCurrentRotationSmoother->smooth(currentEulerPitch, targetEulerPitch);
		pCurrentRotationSmoother->smooth(currentEulerRoll, targetEulerRoll);



		// step one: yaw
		auto quatYaw = SimpleMath::Quaternion::CreateFromAxisAngle(freeCameraData.currentlookDirUp, currentEulerYaw);

		auto totalQuat = quatYaw;

		// transform currentLookDirs by rotations
		freeCameraData.currentlookDirForward = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirForward, totalQuat);
		freeCameraData.currentlookDirForward.Normalize();
		freeCameraData.currentlookDirUp = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirUp, totalQuat);
		freeCameraData.currentlookDirUp.Normalize();
		freeCameraData.currentlookDirRight = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirRight, totalQuat);
		freeCameraData.currentlookDirRight.Normalize();

		// step two: pitch
		auto quatPitch = SimpleMath::Quaternion::CreateFromAxisAngle(freeCameraData.currentlookDirRight, currentEulerPitch);

		totalQuat = quatPitch;

		// transform currentLookDirs by rotations
		freeCameraData.currentlookDirForward = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirForward, totalQuat);
		freeCameraData.currentlookDirForward.Normalize();
		freeCameraData.currentlookDirUp = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirUp, totalQuat);
		freeCameraData.currentlookDirUp.Normalize();
		freeCameraData.currentlookDirRight = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirRight, totalQuat);
		freeCameraData.currentlookDirRight.Normalize();


		// step 3: roll
		auto quatRoll = SimpleMath::Quaternion::CreateFromAxisAngle(freeCameraData.currentlookDirForward, currentEulerRoll);

		totalQuat = quatRoll;

		// transform currentLookDirs by rotations
		freeCameraData.currentlookDirForward = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirForward, totalQuat);
		freeCameraData.currentlookDirForward.Normalize();
		freeCameraData.currentlookDirUp = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirUp, totalQuat);
		freeCameraData.currentlookDirUp.Normalize();
		freeCameraData.currentlookDirRight = SimpleMath::Vector3::Transform(freeCameraData.currentlookDirRight, totalQuat);
		freeCameraData.currentlookDirRight.Normalize();

	}
	float currentEulerYaw;
	float currentEulerPitch;
	float currentEulerRoll;



	float targetEulerYaw;
	float targetEulerPitch;
	float targetEulerRoll;

	NullSmoother<float> nullRotationSmoother;
	LinearSmoother<float> linearRotationSmoother;
	MomentumSmoother<float> momentumRotationSmoother;
	ISmoother<float>* pCurrentRotationSmoother;

	std::shared_ptr<IUpdateRotationTransform> rotationUpdater;

	std::atomic_bool dataInUse = false;

	void onInterpolationTypeChanged(FreeCameraInterpolationTypesEnum& newType)
	{
		ScopedAtomicBool lock(dataInUse);
		switch (newType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentRotationSmoother = &nullRotationSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentRotationSmoother = &linearRotationSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			momentumRotationSmoother.reset();
			pCurrentRotationSmoother = &momentumRotationSmoother;
			break;

		default: PLOG_ERROR << "Invalid smoother enum value!" << newType;
		}
	}

	void onLinearInterpolationFactorChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		linearRotationSmoother.setSmoothRate(newValue);
		// Momentum reuses Snap Factor as its acceleration term, so it tracks the same setting.
		momentumRotationSmoother.setSmoothRate(newValue);
	}

	void onDriftChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		momentumRotationSmoother.setDrag(newValue);
	}

public:

	const SimpleMath::Vector3 getRotationTransformation()
	{
		ScopedAtomicBool lock(dataInUse);
		return SimpleMath::Vector3(currentEulerYaw, currentEulerPitch, currentEulerRoll);
	}

	void setRotationTransformation(float yaw, float pitch, float roll)
	{
		ScopedAtomicBool lock(dataInUse);
		currentEulerYaw = yaw;
		targetEulerYaw = yaw;
		currentEulerPitch = pitch;
		targetEulerPitch = pitch;
		currentEulerRoll = roll;
		targetEulerRoll = roll;
	}

	void setRotationTransformation(SimpleMath::Vector3 rotvec)
	{
		ScopedAtomicBool lock(dataInUse);
		currentEulerYaw = rotvec.x;
		targetEulerYaw = rotvec.x;
		currentEulerPitch = rotvec.y;
		targetEulerPitch = rotvec.y;
		currentEulerRoll = rotvec.z;
		targetEulerRoll = rotvec.z;
	}


	RotationTransformer(std::shared_ptr<IUpdateRotationTransform> rotUpate, std::shared_ptr<BinarySetting<FreeCameraInterpolationTypesEnum>> currentInterpolationType, std::shared_ptr<BinarySetting<float>> currentLinearInterpolationFactor, std::shared_ptr<BinarySetting<float>> currentDrift)
		: 
		rotationUpdater(std::move(rotUpate)),
		currentInterpolationTypeChangedCallback(currentInterpolationType->valueChangedEvent, [this](FreeCameraInterpolationTypesEnum& n) { onInterpolationTypeChanged(n); }),
		currentLinearInterpolationFactorChangedCallback(currentLinearInterpolationFactor->valueChangedEvent, [this](float& n) { onLinearInterpolationFactorChanged(n); }),
		currentDriftChangedCallback(currentDrift->valueChangedEvent, [this](float& n) { onDriftChanged(n); }),
		linearRotationSmoother(currentLinearInterpolationFactor->GetValue()),
		momentumRotationSmoother(currentLinearInterpolationFactor->GetValue(), currentDrift->GetValue())
	{
		auto curInterpolationType = currentInterpolationType->GetValue();
		switch (curInterpolationType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentRotationSmoother = &nullRotationSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentRotationSmoother = &linearRotationSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			pCurrentRotationSmoother = &momentumRotationSmoother;
			break;

		default:  throw HCMInitException(std::format("Invalid smoother enum value! {}", curInterpolationType));
		}
	}

	
	

	void transformRotation(FreeCameraData& freeCameraData, float frameDelta)
	{
		ScopedAtomicBool lock(dataInUse);
		rotationUpdater->updateRotationTransform(freeCameraData, frameDelta, targetEulerYaw, targetEulerPitch, targetEulerRoll);
		applyRotationTransform(freeCameraData, frameDelta);
	}



};

class PositionTransformer
{
private:

	ScopedCallback< eventpp::CallbackList<void(FreeCameraInterpolationTypesEnum&)>> currentInterpolationTypeChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentLinearInterpolationFactorChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentDriftChangedCallback;

	void applyPositionTransform(FreeCameraData& freeCameraData, float frameDelta)
	{


		pCurrentPositionSmoother->smooth(currentPositionTransformation, targetPositionTransformation);

		freeCameraData.currentPosition = freeCameraData.currentPosition + currentPositionTransformation;
	}



	NullSmoother<SimpleMath::Vector3> nullPositionSmoother;
	LinearSmoother<SimpleMath::Vector3> linearPositionSmoother;
	MomentumSmoother<SimpleMath::Vector3> momentumPositionSmoother;
	ISmoother<SimpleMath::Vector3> * pCurrentPositionSmoother;
	std::shared_ptr<IUpdatePositionTransform> positionUpdater;

	SimpleMath::Vector3 currentPositionTransformation;
	SimpleMath::Vector3 targetPositionTransformation;

	std::atomic_bool dataInUse = false;

	void onInterpolationTypeChanged(FreeCameraInterpolationTypesEnum& newType)
	{
		ScopedAtomicBool lock(dataInUse);
		switch (newType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentPositionSmoother = &nullPositionSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentPositionSmoother = &linearPositionSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			momentumPositionSmoother.reset();
			pCurrentPositionSmoother = &momentumPositionSmoother;
			break;

		default: PLOG_ERROR << "Invalid smoother enum value!" << newType;
		}
	}

	void onLinearInterpolationFactorChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		linearPositionSmoother.setSmoothRate(newValue);
		// Momentum reuses Snap Factor as its acceleration term, so it tracks the same setting.
		momentumPositionSmoother.setSmoothRate(newValue);
	}

	void onDriftChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		momentumPositionSmoother.setDrag(newValue);
	}

public:

	const SimpleMath::Vector3 getPositionTransformation()
	{
		ScopedAtomicBool lock(dataInUse);
		return currentPositionTransformation;
	}

	void setPositionTransformation(SimpleMath::Vector3 newPos)
	{
		ScopedAtomicBool lock(dataInUse);
		currentPositionTransformation = newPos;
		targetPositionTransformation = newPos;
	}


	PositionTransformer (std::shared_ptr<IUpdatePositionTransform> posUpdate, std::shared_ptr<BinarySetting<FreeCameraInterpolationTypesEnum>> currentInterpolationType, std::shared_ptr<BinarySetting<float>> currentLinearInterpolationFactor, std::shared_ptr<BinarySetting<float>> currentDrift)
		: positionUpdater(std::move(posUpdate)),
		currentInterpolationTypeChangedCallback(currentInterpolationType->valueChangedEvent, [this](FreeCameraInterpolationTypesEnum& n) { onInterpolationTypeChanged(n); }),
		currentLinearInterpolationFactorChangedCallback(currentLinearInterpolationFactor->valueChangedEvent, [this](float& n) { onLinearInterpolationFactorChanged(n); }),
		currentDriftChangedCallback(currentDrift->valueChangedEvent, [this](float& n) { onDriftChanged(n); }),
		linearPositionSmoother(currentLinearInterpolationFactor->GetValue()),
		momentumPositionSmoother(currentLinearInterpolationFactor->GetValue(), currentDrift->GetValue())
	{
		PLOG_DEBUG << "constructing PositionTransformer";

		auto curInterpolationType = currentInterpolationType->GetValue();
		switch (curInterpolationType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentPositionSmoother = &nullPositionSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentPositionSmoother = &linearPositionSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			pCurrentPositionSmoother = &momentumPositionSmoother;
			break;

		default:  throw HCMInitException(std::format("Invalid smoother enum value! {}", curInterpolationType));
		}

	}



	void transformPosition(FreeCameraData& freeCameraData, float frameDelta)
	{
		ScopedAtomicBool lock(dataInUse);
		positionUpdater->updatePositionTransform(freeCameraData, frameDelta, targetPositionTransformation);
		applyPositionTransform(freeCameraData, frameDelta);
	}


};

class FOVTransformer
{
private:
	ScopedCallback< eventpp::CallbackList<void(FreeCameraInterpolationTypesEnum&)>> currentInterpolationTypeChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentLinearInterpolationFactorChangedCallback;
	ScopedCallback< eventpp::CallbackList<void(float&)>> currentDriftChangedCallback;


	void applyFOVTransform(FreeCameraData& freeCameraData, float frameDelta, float& targetFOV)
	{

		pCurrentFOVSmoother->smooth(currentFOV, targetFOV);
	}



	float currentFOV = 90;



	NullSmoother<float> nullFOVSmoother;
	LinearSmoother<float> linearFOVSmoother;
	MomentumSmoother<float> momentumFOVSmoother;
	ISmoother<float>* pCurrentFOVSmoother;
	std::shared_ptr<IUpdateFOVTransform> fovUpdater;

	std::atomic_bool dataInUse = false;

	void onInterpolationTypeChanged(FreeCameraInterpolationTypesEnum& newType)
	{
		ScopedAtomicBool lock(dataInUse);
		switch (newType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentFOVSmoother = &nullFOVSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentFOVSmoother = &linearFOVSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			momentumFOVSmoother.reset();
			pCurrentFOVSmoother = &momentumFOVSmoother;
			break;

		default: PLOG_ERROR << "Invalid smoother enum value!" << newType;
		}
	}

	void onLinearInterpolationFactorChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		linearFOVSmoother.setSmoothRate(newValue);
		// Momentum reuses Snap Factor as its acceleration term, so it tracks the same setting.
		momentumFOVSmoother.setSmoothRate(newValue);
	}

	void onDriftChanged(float& newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		momentumFOVSmoother.setDrag(newValue);
	}

public:


	void setFOV(float newValue)
	{
		ScopedAtomicBool lock(dataInUse);
		currentFOV = newValue;
	}

	FOVTransformer(std::shared_ptr<IUpdateFOVTransform> fovUpate, std::shared_ptr<BinarySetting<FreeCameraInterpolationTypesEnum>> currentInterpolationType, std::shared_ptr<BinarySetting<float>> currentLinearInterpolationFactor, std::shared_ptr<BinarySetting<float>> currentDrift)
		:
		fovUpdater(std::move(fovUpate)),
		currentInterpolationTypeChangedCallback(currentInterpolationType->valueChangedEvent, [this](FreeCameraInterpolationTypesEnum& n) { onInterpolationTypeChanged(n); }),
		currentLinearInterpolationFactorChangedCallback(currentLinearInterpolationFactor->valueChangedEvent, [this](float& n) { onLinearInterpolationFactorChanged(n); }),
		currentDriftChangedCallback(currentDrift->valueChangedEvent, [this](float& n) { onDriftChanged(n); }),
		linearFOVSmoother(currentLinearInterpolationFactor->GetValue()),
		momentumFOVSmoother(currentLinearInterpolationFactor->GetValue(), currentDrift->GetValue())
	{
		auto curInterpolationType = currentInterpolationType->GetValue();
		switch (curInterpolationType)
		{
		case FreeCameraInterpolationTypesEnum::None:
			pCurrentFOVSmoother = &nullFOVSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Linear:
			pCurrentFOVSmoother = &linearFOVSmoother;
			break;

		case FreeCameraInterpolationTypesEnum::Momentum:
			pCurrentFOVSmoother = &momentumFOVSmoother;
			break;

		default:  throw HCMInitException(std::format("Invalid smoother enum value! {}", curInterpolationType));
		}
	}

	void transformFOV(FreeCameraData& freeCameraData, float frameDelta, float& targetFOV)
	{
		ScopedAtomicBool lock(dataInUse);
		fovUpdater->updateFOVTransform(freeCameraData, frameDelta, targetFOV);
		applyFOVTransform(freeCameraData, frameDelta, targetFOV);
	}


};