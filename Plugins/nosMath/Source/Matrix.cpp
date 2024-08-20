// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <tinyexpr.h>
#include <Nodos/PluginHelpers.hpp>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace nos::math
{
struct TransformNodeContext : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* execParams) override
	{
		nos::NodeExecuteParams params(execParams);
		auto* rhs = params.GetPinData<fb::mat4>(NOS_NAME("A"));
		auto* lhs = params.GetPinData<fb::mat4>(NOS_NAME("B"));
		auto* out = params.GetPinData<fb::mat4>(NOS_NAME("Result"));

		std::array o = { &out->mutable_x(), &out->mutable_y(), &out->mutable_z(), &out->mutable_w() };
		std::array l = { &lhs->mutable_x(), &lhs->mutable_y(), &lhs->mutable_z(), &lhs->mutable_w() };
		std::array r = { &rhs->mutable_x(), &rhs->mutable_y(), &rhs->mutable_z(), &rhs->mutable_w() };

		for (int i = 0; i < 4; i++)
		{
			o[i]->mutate_x(r[i]->x() * l[0]->x() + r[i]->y() * l[1]->x() + r[i]->z() * l[2]->x() + r[i]->w() * l[3]->x());
			o[i]->mutate_y(r[i]->x() * l[0]->y() + r[i]->y() * l[1]->y() + r[i]->z() * l[2]->y() + r[i]->w() * l[3]->y());
			o[i]->mutate_z(r[i]->x() * l[0]->z() + r[i]->y() * l[1]->z() + r[i]->z() * l[2]->z() + r[i]->w() * l[3]->z());
			o[i]->mutate_w(r[i]->x() * l[0]->w() + r[i]->y() * l[1]->w() + r[i]->z() * l[2]->w() + r[i]->w() * l[3]->w());
		}

		return NOS_RESULT_SUCCESS;
	}
};

struct ToTransformMatrixNodeContext : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* execParams) override
	{
		nos::NodeExecuteParams args(execParams);
		auto* xform = args.GetPinData<fb::Transform>(NOS_NAME("Transform"));
		auto* out = args.GetPinData<fb::mat4>(NOS_NAME("Matrix"));

		auto pos = xform->position();
		auto rot = xform->rotation();
		auto scale = xform->scale();

		auto res = glm::dmat4(1.0f);
		res = glm::scale(res, glm::dvec3(scale.x(), scale.y(), scale.z()));
		res = glm::rotate(res, glm::radians(rot.x()), glm::dvec3(1, 0, 0));
		res = glm::rotate(res, glm::radians(rot.y()), glm::dvec3(0, 1, 0));
		res = glm::rotate(res, glm::radians(rot.z()), glm::dvec3(0, 0, 1));
		res = glm::translate(res, glm::dvec3(pos.x(), pos.y(), pos.z()));

		glm::mat4 resf(res);
		*out = ((fb::mat4&)resf);

		return NOS_RESULT_SUCCESS;
	}
};

enum class MatOp
{
	Inverse,
	Transpose
};

template <MatOp OpType>
struct MatrixOperationNodeContext : NodeContext
{
	MatrixOperationNodeContext(const nosFbNode* node) : NodeContext(node)
	{
		for (auto& [id, pin] : Pins)
		{
			if (pin.TypeName != NOS_NAME("nos.fb.Void"))
			{
				TypeName = pin.TypeName;
				break;
			}
		}
	}
	
	std::optional<nos::Name> TypeName = std::nullopt;
	
	template <typename T>
	void ApplyOperation(nos::NodeExecuteParams& params)
	{
		auto* in = params.GetPinData<T>(NOS_NAME("In"));
		auto* out = params.GetPinData<T>(NOS_NAME("Out"));
#define OP(ty, glmty)								\
			if constexpr (std::is_same_v<T, ty>)				\
			{													\
				glmty& inglm = reinterpret_cast<glmty&>(*in);	\
				glmty& outglm = reinterpret_cast<glmty&>(*out); \
				if constexpr (OpType == MatOp::Inverse)		\
					outglm = glm::inverse(inglm);				\
				if constexpr (OpType == MatOp::Transpose)	\
					outglm = glm::transpose(inglm);				\
			}
		OP(fb::mat4, glm::mat4)
		OP(fb::mat3, glm::mat3)
		OP(fb::mat2, glm::mat2)
		OP(fb::mat4d, glm::dmat4)
		OP(fb::mat3d, glm::dmat3)
		OP(fb::mat2d, glm::dmat2)
#undef OP
	}
	
	nosResult ExecuteNode(nosNodeExecuteParams* execParams) override
	{
		nos::NodeExecuteParams params(execParams);

		if (TypeName == NOS_NAME("nos.fb.mat4"))
			ApplyOperation<fb::mat4>(params);
		else if (TypeName == NOS_NAME("nos.fb.mat3"))
			ApplyOperation<fb::mat3>(params);
		else if (TypeName == NOS_NAME("nos.fb.mat2"))
			ApplyOperation<fb::mat2>(params);
		else if (TypeName == NOS_NAME("nos.fb.mat4d"))
			ApplyOperation<fb::mat4d>(params);
		else if (TypeName == NOS_NAME("nos.fb.mat3d"))
			ApplyOperation<fb::mat3d>(params);
		else if (TypeName == NOS_NAME("nos.fb.mat2d"))
			ApplyOperation<fb::mat2d>(params);

		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(const nosPinUpdate* pinUpdate) override
	{
		if (!TypeName && pinUpdate->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
			TypeName = pinUpdate->TypeName;
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		return (params->IncomingTypeName == NOS_NAME("nos.fb.mat4")
			|| params->IncomingTypeName == NOS_NAME("nos.fb.mat3")
			|| params->IncomingTypeName == NOS_NAME("nos.fb.mat2")
			|| params->IncomingTypeName == NOS_NAME("nos.fb.mat4d")
			|| params->IncomingTypeName == NOS_NAME("nos.fb.mat3d")
			|| params->IncomingTypeName == NOS_NAME("nos.fb.mat2d"))
			? NOS_RESULT_SUCCESS : NOS_RESULT_FAILED;
	}
	
};

void RegisterTransform(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.math.Transform"), TransformNodeContext, fn)
}

void RegisterToTransformMatrix(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.math.ToTransformMatrix"), ToTransformMatrixNodeContext, fn)
}

void RegisterInverse(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.math.Inverse"), MatrixOperationNodeContext<MatOp::Inverse>, fn)
}

void RegisterTranspose(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.math.Transpose"), MatrixOperationNodeContext<MatOp::Transpose>, fn)
}

}  // namespace nos::math
