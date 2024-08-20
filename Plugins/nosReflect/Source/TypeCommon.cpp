// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"
namespace flatbuffers
{

EnumVal* EnumDef::ReverseLookup(int64_t enum_idx,
	bool skip_union_default) const {
	auto skip_first = static_cast<int>(is_union && skip_union_default);
	for (auto it = Vals().begin() + skip_first; it != Vals().end(); ++it) {
		if ((*it)->GetAsInt64() == enum_idx) { return *it; }
	}
	return nullptr;
}
}
namespace nos::reflect
{

NOS_REGISTER_NAME(Output)
NOS_REGISTER_NAME(Input)
NOS_REGISTER_NAME_SPACED(VOID, "nos.fb.Void")


void CopyInline(
	flatbuffers::FlatBufferBuilder& fbb, uint16_t offset, const u8* data, size_t align, size_t size)
{
	fbb.Align(align);
	fbb.PushBytes(data, size);
	fbb.TrackField(offset, fbb.GetSize());
}

void CopyInline2(flatbuffers::FlatBufferBuilder& fbb, const flatbuffers::FieldDef* fielddef,
	const flatbuffers::Table* table, size_t align, size_t size) {
	fbb.Align(align);
	fbb.PushBytes(table->GetStruct<const uint8_t*>(fielddef->value.offset), size);
	fbb.TrackField(fielddef->value.offset, fbb.GetSize());
}

const flatbuffers::StructDef* GetUnionType(
	const flatbuffers::StructDef* parent,
	const flatbuffers::FieldDef* unionfield, 
	const flatbuffers::Table* table) {
	auto enumdef = unionfield->value.type.enum_def;
	// TODO: this is clumsy and slow, but no other way to find it?
	auto type_field = parent->fields.Lookup(unionfield->name + "_type");
	
	FLATBUFFERS_ASSERT(type_field);
	auto union_type = table->GetField<u8>(type_field->value.offset, 0);
	auto enumval = enumdef->ReverseLookup(union_type);
	return enumval->union_type.struct_def;
}

flatbuffers::uoffset_t CopyTable2(
	flatbuffers::FlatBufferBuilder& fbb,
	const flatbuffers::StructDef* objectdef,
	const flatbuffers::Table* table)
{
	// Before we can construct the table, we have to first generate any
	// subobjects, and collect their offsets.
	std::vector<flatbuffers::uoffset_t> offsets;

	for (auto field : objectdef->fields.vec)
	{
		// Skip if field is not present in the source.
		if (!table->CheckField(field->value.offset)) continue;
		flatbuffers::uoffset_t offset = 0;
		switch (field->value.type.base_type) {
		case flatbuffers::BASE_TYPE_STRING: {
			offset = fbb.CreateString(table->GetPointer<const flatbuffers::String*>(field->value.offset)).o;
			break;
		}
		case flatbuffers::BASE_TYPE_STRUCT: {
			if (!field->value.type.struct_def->fixed) {
				offset = CopyTable2(fbb, field->value.type.struct_def, table->GetPointer<flatbuffers::Table*>(field->value.offset));
			}
			break;
		}
		case flatbuffers::BASE_TYPE_UNION: {
			offset = CopyTable2(fbb, GetUnionType(objectdef, field, table), table->GetPointer<flatbuffers::Table*>(field->value.offset));
			break;
		}
		case flatbuffers::BASE_TYPE_VECTOR: {
			auto vec =
				table->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>> *>(field->value.offset);
			auto element_base_type = field->value.type.element;
			auto elemobjectdef = element_base_type == flatbuffers::BASE_TYPE_STRUCT ? field->value.type.struct_def : 0;
			
			switch (element_base_type) {
			case flatbuffers::BASE_TYPE_STRING: {
				std::vector<flatbuffers::Offset<const flatbuffers::String*>> elements(vec->size());
				auto vec_s = reinterpret_cast<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> *>(vec);
				for (flatbuffers::uoffset_t i = 0; i < vec_s->size(); i++) {
					elements[i] = fbb.CreateString(vec_s->Get(i)).o;
				}
				offset = fbb.CreateVector(elements).o;
				break;
			}
			case flatbuffers::BASE_TYPE_STRUCT: {
				if (!elemobjectdef->fixed) {
					std::vector<flatbuffers::Offset<const flatbuffers::Table*>> elements(vec->size());
					for (flatbuffers::uoffset_t i = 0; i < vec->size(); i++) {
						elements[i] = CopyTable2(fbb, elemobjectdef, vec->Get(i));
					}
					offset = fbb.CreateVector(elements).o;
					break;
				}
			}
								FLATBUFFERS_FALLTHROUGH();  // fall thru
			default: {                    // Scalars and structs.
				auto element_size = SizeOf(element_base_type);
				auto element_alignment = element_size; // For primitive elements
				if (elemobjectdef && elemobjectdef->fixed)
					element_size = elemobjectdef->bytesize;
				fbb.StartVector(vec->size(), element_size, element_alignment);
				fbb.PushBytes(vec->Data(), element_size * vec->size());
				offset = fbb.EndVector(vec->size());
				break;
			}
			}
			break;
		}
		default:  // Scalars.
			break;
		}
		if (offset) { offsets.push_back(offset); }
	}
	// Now we can build the actual table from either offsets or scalar data.
	auto start = objectdef->fixed ? fbb.StartStruct(objectdef->minalign)
		: fbb.StartTable();
	size_t offset_idx = 0;

	for (auto field : objectdef->fields.vec)
	{
		if (!table->CheckField(field->value.offset)) continue;
		auto base_type = field->value.type.base_type;
		switch (base_type) {
		case flatbuffers::BASE_TYPE_STRUCT: {
			if (field->value.type.struct_def->fixed) {
				CopyInline2(fbb, field, table, field->value.type.struct_def->minalign, field->value.type.struct_def->bytesize);
				break;
			}
		}
							FLATBUFFERS_FALLTHROUGH();  // fall thru
		case flatbuffers::BASE_TYPE_UNION:
		case flatbuffers::BASE_TYPE_STRING:
		case flatbuffers::BASE_TYPE_VECTOR:
			fbb.AddOffset(field->value.offset, flatbuffers::Offset<void>(offsets[offset_idx++]));
			break;
		default: {  // Scalars.
			auto size = SizeOf(base_type);
			CopyInline2(fbb, field, table, size, size);
			break;
		}
		}
	}
	FLATBUFFERS_ASSERT(offset_idx == offsets.size());
	if (objectdef->fixed) {
		fbb.ClearOffsets();
		return fbb.EndStruct();
	}
	else {
		return fbb.EndTable(start);
	}
}

void CopyInline(flatbuffers::FlatBufferBuilder& fbb, decltype(nosTypeInfo::Fields) fielddef,
    const flatbuffers::Table* table, size_t align, size_t size) {
    fbb.Align(align);
    fbb.PushBytes(table->GetStruct<const uint8_t*>(fielddef->Offset), size);
    fbb.TrackField(fielddef->Offset, fbb.GetSize());
}

flatbuffers::uoffset_t CopyTable(
	flatbuffers::FlatBufferBuilder& fbb,
	const nosTypeInfo* type,
	const flatbuffers::Table* table)
{
	// Before we can construct the table, we have to first generate any
	// subobjects, and collect their offsets.
	std::vector<flatbuffers::uoffset_t> offsets;

    for(int i = 0; i < type->FieldCount; ++i)
	{
        auto field = &type->Fields[i];
		// Skip if field is not present in the source.
		if (!table->CheckField(field->Offset)) continue;
		flatbuffers::uoffset_t offset = 0;
		switch (field->Type->BaseType) {
		case NOS_BASE_TYPE_STRING: {
			offset = fbb.CreateString(table->GetPointer<const flatbuffers::String*>(field->Offset)).o;
			break;
		}
		case NOS_BASE_TYPE_STRUCT: {
			if (!field->Type->ByteSize) {
				offset = CopyTable(fbb, field->Type, table->GetPointer<flatbuffers::Table*>(field->Offset));
			}
			break;
		}
		//case NOS_BASE_TYPE_UNION: {
		//	offset = CopyTable2(fbb, GetUnionType(objectdef, field, table), table->GetPointer<flatbuffers::Table*>(field->Offset));
		//	break;
		//}
		case NOS_BASE_TYPE_ARRAY: {
			auto vec =
				table->GetPointer<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>> *>(field->Offset);
			auto element_base_type = field->Type->ElementType->BaseType;
			// auto elemobjectdef = element_base_type == NOS_BASE_TYPE_STRUCT ? field->Type->struct_def : 0;
			
			switch (element_base_type) {
			case NOS_BASE_TYPE_STRING: {
				std::vector<flatbuffers::Offset<const flatbuffers::String*>> elements(vec->size());
				auto vec_s = reinterpret_cast<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> *>(vec);
				for (flatbuffers::uoffset_t i = 0; i < vec_s->size(); i++) {
					elements[i] = fbb.CreateString(vec_s->Get(i)).o;
				}
				offset = fbb.CreateVector(elements).o;
				break;
			}
			case NOS_BASE_TYPE_STRUCT: {
				if (!field->Type->ElementType->ByteSize) {
					std::vector<flatbuffers::Offset<const flatbuffers::Table*>> elements(vec->size());
					for (flatbuffers::uoffset_t i = 0; i < vec->size(); i++) {
						elements[i] = CopyTable(fbb, field->Type->ElementType, vec->Get(i));
					}
					offset = fbb.CreateVector(elements).o;
					break;
				}
			}
								FLATBUFFERS_FALLTHROUGH();  // fall thru
			default: {                    // Scalars and structs.
				fbb.StartVector(vec->size(), field->Type->ByteSize, field->Type->Alignment);
				fbb.PushBytes(vec->Data(), field->Type->ByteSize * vec->size());
				offset = fbb.EndVector(vec->size());
				break;
			}
			}
			break;
		}
		default:  // Scalars.
			break;
		}
		if (offset) { offsets.push_back(offset); }
	}
	// Now we can build the actual table from either offsets or scalar data.
	auto start = type->ByteSize ? fbb.StartStruct(type->Alignment)
		: fbb.StartTable();
	size_t offset_idx = 0;

    for (int i = 0; i < type->FieldCount; ++i)
    {
        auto field = &type->Fields[i];
		if (!table->CheckField(field->Offset)) continue;
		auto base_type = field->Type->BaseType;
		switch (base_type) {
		case NOS_BASE_TYPE_STRUCT: {
			if (field->Type->ByteSize) {
				CopyInline(fbb, field, table, field->Type->Alignment, field->Type->ByteSize);
				break;
			}
		}
		// case NOS_BASE_TYPE_UNION:
		case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_ARRAY:
			fbb.AddOffset(field->Offset, flatbuffers::Offset<void>(offsets[offset_idx++]));
			break;
		default: {  // Scalars.
            CopyInline(fbb, field, table, field->Type->Alignment, field->Type->ByteSize);
			break;
		}
		}
	}
	FLATBUFFERS_ASSERT(offset_idx == offsets.size());
	if (type->ByteSize) {
		fbb.ClearOffsets();
		return fbb.EndStruct();
	}
	else {
		return fbb.EndTable(start);
	}
}

flatbuffers::uoffset_t CopyArgs(
	flatbuffers::FlatBufferBuilder& fbb,
	const nosTypeInfo* type,
	NodeExecuteParams& table)
{
	if(NOS_BASE_TYPE_STRUCT != type->BaseType)
		return 0;
	// Before we can construct the table, we have to first generate any
	// subobjects, and collect their offsets.
	std::vector<flatbuffers::uoffset_t> offsets;

	for (int i = 0; i < type->FieldCount; ++i)
	{
		auto field = &type->Fields[type->FieldCount-i-1];
		// Skip if field is not present in the source.
		if (!table.contains(field->Name)) continue;

		auto data = table[field->Name].Data->Data;

		flatbuffers::uoffset_t offset = 0;
		switch (field->Type->BaseType) {
		case NOS_BASE_TYPE_STRING: {
			offset = fbb.CreateString((const char*)data).o;
			break;
		}
		case NOS_BASE_TYPE_STRUCT: {
			if (!field->Type->ByteSize) {
				offset = CopyTable(fbb, field->Type, flatbuffers::GetRoot<flatbuffers::Table>(data));
			}
			break;
		}
								//case NOS_BASE_TYPE_UNION: {
								//	offset = CopyTable2(fbb, GetUnionType(objectdef, field, table), table->GetPointer<flatbuffers::Table*>(field->Offset));
								//	break;
								//}
		case NOS_BASE_TYPE_ARRAY: {
			auto vec = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*)(data);
			auto element_base_type = field->Type->ElementType->BaseType;
			// auto elemobjectdef = element_base_type == NOS_BASE_TYPE_STRUCT ? field->Type->struct_def : 0;

			switch (element_base_type) {
			case NOS_BASE_TYPE_STRING: {
				std::vector<flatbuffers::Offset<const flatbuffers::String*>> elements(vec->size());
				auto vec_s = reinterpret_cast<const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> *>(vec);
				for (flatbuffers::uoffset_t i = 0; i < vec_s->size(); i++) {
					elements[i] = fbb.CreateString(vec_s->Get(i)).o;
				}
				offset = fbb.CreateVector(elements).o;
				break;
			}
			case NOS_BASE_TYPE_STRUCT: {
				if (!field->Type->ElementType->ByteSize) {
					std::vector<flatbuffers::Offset<const flatbuffers::Table*>> elements(vec->size());
					for (flatbuffers::uoffset_t i = 0; i < vec->size(); i++) {
						elements[i] = CopyTable(fbb, field->Type->ElementType, vec->Get(i));
					}
					offset = fbb.CreateVector(elements).o;
					break;
				}
			}
									FLATBUFFERS_FALLTHROUGH();  // fall thru
			default: {                    // Scalars and structs.
				fbb.StartVector(vec->size(), field->Type->ByteSize, field->Type->Alignment);
				fbb.PushBytes(vec->Data(), field->Type->ByteSize * vec->size());
				offset = fbb.EndVector(vec->size());
				break;
			}
			}
			break;
		}
		default:  // Scalars.
			break;
		}
		if (offset) { offsets.push_back(offset); }
	}
	// Now we can build the actual table from either offsets or scalar data.
	auto start = type->ByteSize ? fbb.StartStruct(type->Alignment)
		: fbb.StartTable();
	size_t offset_idx = 0;

	for (int i = 0; i < type->FieldCount; ++i)
	{
		auto field = &type->Fields[type->FieldCount-i-1];
		if (!table.contains(field->Name)) continue;

		auto data = table[field->Name].Data->Data;
		auto base_type = field->Type->BaseType;
		switch (base_type) {
		case NOS_BASE_TYPE_STRUCT: {
			if (field->Type->ByteSize) {
				fbb.Align(field->Type->Alignment);
				fbb.PushBytes((u8*)data, field->Type->ByteSize);
				fbb.TrackField(field->Offset, fbb.GetSize());
				break;
			}
		}
								// case NOS_BASE_TYPE_UNION:
		case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_ARRAY:
			fbb.AddOffset(field->Offset, flatbuffers::Offset<void>(offsets[offset_idx++]));
			break;
		default: {  // Scalars.
			fbb.Align(field->Type->Alignment);
			fbb.PushBytes((u8*)data, field->Type->ByteSize);
			fbb.TrackField(field->Offset, fbb.GetSize());
			break;
		}
		}
	}
	FLATBUFFERS_ASSERT(offset_idx == offsets.size());
	if (type->ByteSize) {
		fbb.ClearOffsets();
		return fbb.EndStruct();
	}
	else {
		return fbb.EndTable(start);
	}
}

flatbuffers::uoffset_t GenerateOffset(
    flatbuffers::FlatBufferBuilder& fbb,
    const nosTypeInfo* type,
    const void* data)
{
    if(type->ByteSize) 
        return 0;
    switch (type->BaseType)
    {
    case NOS_BASE_TYPE_STRUCT:
        return CopyTable(fbb, type, flatbuffers::GetRoot<flatbuffers::Table>(data));
    case NOS_BASE_TYPE_STRING:
        return fbb.CreateString((const flatbuffers::String*)data).o;
    case NOS_BASE_TYPE_ARRAY: {
        auto vec = (flatbuffers::Vector<void*>*)(data);
        if(type->ElementType->ByteSize)
        {
            fbb.StartVector(vec->size(), type->ElementType->ByteSize, 1);
            fbb.PushBytes(vec->Data(), type->ElementType->ByteSize * vec->size());
            return fbb.EndVector(vec->size());
        }
        std::vector<flatbuffers::uoffset_t> elements(vec->size());
        for (int i = 0; i < vec->size(); i++) {
            elements[i] = GenerateOffset(fbb, type->ElementType, vec->Get(i));
        }
        return fbb.CreateVector(elements).o;
    }
    }
    return 0;
}

std::vector<u8> GenerateBuffer(
	const nosTypeInfo* type,
	const void* data)
{
	if (type->ByteSize)
	{
		if (data) return std::vector<u8>{(u8*)data, (u8*)data + type->ByteSize};
		return std::vector<u8>(type->ByteSize);
	}
	if(!data) return {};
    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(flatbuffers::Offset<u8>(GenerateOffset(fbb, type, data)));
    return nos::Buffer(fbb.Release());
}

flatbuffers::uoffset_t GenerateVector(
	flatbuffers::FlatBufferBuilder& fbb, 
	const nosTypeInfo* type, 
	std::vector<const void*> values)
{
	flatbuffers::uoffset_t offset = 0;
	if (type->ByteSize)
	{
		fbb.StartVector(values.size(), type->ByteSize, 1);
		for (u32 i = values.size(); i != 0; --i)
			fbb.PushBytes((u8*)values[i-1], type->ByteSize);
		offset = fbb.EndVector(values.size());
	}
	else
	{
		switch (type->BaseType)
		{
		case NOS_BASE_TYPE_STRING: {
			std::vector<flatbuffers::Offset<flatbuffers::String>> elements;
			for (int i = 0; i < values.size(); ++i)
				elements.push_back(fbb.CreateString((const char*)values[i]));
			offset = fbb.CreateVector(elements).o;
			break;
		}	
		case NOS_BASE_TYPE_STRUCT: {
			std::vector<flatbuffers::Offset<const flatbuffers::Table*>> elements;
			for (int i = 0; i < values.size(); ++i)
				elements.push_back(CopyTable(fbb, type, flatbuffers::GetRoot<flatbuffers::Table>(values[i])));
			offset = fbb.CreateVector(elements).o;
			break;
		}
		case NOS_BASE_TYPE_ARRAY:
		default: {                    // Scalars and structs.
			assert(0);
		}
		}
	}
	return offset;
}


std::vector<u8> GenerateVector(const nosTypeInfo* type, std::vector<const void*> inputs)
{
	flatbuffers::FlatBufferBuilder fbb;
	fbb.Finish(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>(GenerateVector(fbb, type, std::move(inputs))));
	auto buf = fbb.Release();
	return std::vector<u8>{flatbuffers::GetMutableRoot<u8>(buf.data()), buf.data()+buf.size()};
}

} // namespace nos::engine