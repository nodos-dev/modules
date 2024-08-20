/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include <Nodos/PluginHelpers.hpp>
namespace nos::reflect
{
extern nos::Name NSN_Output;
extern nos::Name NSN_Input;
extern nos::Name NSN_VOID;

void CopyInline(flatbuffers::FlatBufferBuilder& fbb, decltype(nosTypeInfo::Fields) fielddef,
    const flatbuffers::Table* table, size_t align, size_t size);

void CopyInline(
	flatbuffers::FlatBufferBuilder& fbb, uint16_t offset, const u8* data, size_t align, size_t size);

void CopyInline2(flatbuffers::FlatBufferBuilder& fbb, const flatbuffers::FieldDef* fielddef,
	const flatbuffers::Table* table, size_t align, size_t size);

const flatbuffers::StructDef* GetUnionType(
	const flatbuffers::StructDef* parent,
	const flatbuffers::FieldDef* unionfield, 
	const flatbuffers::Table* table);

flatbuffers::uoffset_t CopyTable2(
	flatbuffers::FlatBufferBuilder& fbb,
	const flatbuffers::StructDef* objectdef,
	const flatbuffers::Table* table);

flatbuffers::uoffset_t CopyTable(
	flatbuffers::FlatBufferBuilder& fbb,
	const nosTypeInfo* type,
	const flatbuffers::Table* table);

flatbuffers::uoffset_t CopyArgs(
	flatbuffers::FlatBufferBuilder& fbb,
	const nosTypeInfo* type,
	NodeExecuteParams& table);

flatbuffers::uoffset_t GenerateOffset(
    flatbuffers::FlatBufferBuilder& fbb,
    const nosTypeInfo* type,
    const void* data);

std::vector<u8> GenerateBuffer(
    const nosTypeInfo* type,
    const void* data);

std::vector<u8> GenerateVector(
	const nosTypeInfo* type, 
	std::vector<const void*> inputs);

}