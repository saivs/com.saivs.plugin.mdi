#pragma once

// -----------------------------------------------------------------------
// Dependency-free DXBC/DXIL input-signature parser.
//
// Shared by MDIBackend_D3D11.cpp and MDIBackend_D3D12.cpp. Two reasons this
// exists instead of relying on D3DReflect:
//
//   1. D3D11: Unity often passes a signature-only blob (from
//      D3DGetInputSignatureBlob) to CreateInputLayout. D3DReflect rejects
//      those with E_INVALIDARG because it wants the full SHEX chunk.
//
//   2. D3D12 + shader model 6: Unity ships DXIL, and d3dcompiler's
//      D3DReflect only understands DXBC (SM <= 5.1). DXIL containers reuse
//      the 'DXBC' container fourcc and still carry an input-signature part,
//      so parsing the container directly works for both. This avoids taking
//      a dependency on dxcompiler.dll, which does NOT ship with Windows.
//
// Container layout: magic(4) + md5(16) + version(4) + totalSize(4) +
// chunkCount(4), then chunkCount 32-bit chunk offsets, then chunks of
// { fourCC(4), size(4), payload }.
//
// The input-signature chunk payload is { numElements(4), reserved(4) }
// followed by fixed-size element records. Element layout depends on which
// compiler produced the container:
//
//   ISGN (fxc, 24B): nameOffset(0) semanticIndex(4) sysValue(8)
//                    componentType(12) register(16) mask(20) rwMask(21)
//   ISG1 (legacy, 28B): as ISGN + minPrecision(24)
//   ISG1 (dxc,   32B): stream(0) nameOffset(4) semanticIndex(8)
//                      sysValue(12) componentType(16) register(20)
//                      mask(24) rwMask(25) pad(26) minPrecision(28)
//
// The dxc form is DxilProgramSignatureElement and leads with Stream, so the
// name offset is NOT at byte 0. Rather than guess from the fourCC (both dxc
// and older toolchains emit ISG1), we validate each candidate layout against
// the chunk and use the first one whose name offsets all resolve to
// plausible NUL-terminated semantic names.
//
// Semantic names are stored as byte offsets relative to the chunk payload.
// -----------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace MDIDxbc
{

// Matches D3D_REGISTER_COMPONENT_UINT32 in the signature blob encoding.
inline constexpr uint32_t kComponentTypeUInt32 = 1u;

struct SignatureElementLayout
{
		uint32_t stride;
		uint32_t nameOffsetField;
		uint32_t semanticIndexField;
		uint32_t componentTypeField;
};

// Ordered widest-first: a 32B dxc chunk can also satisfy the 24B/28B bounds
// check when numElements is small, so the more specific layout must win.
inline constexpr SignatureElementLayout kSignatureLayouts[] = {
		{ 32u, 4u, 8u, 16u },  // ISG1, dxc / DxilProgramSignatureElement
		{ 28u, 0u, 4u, 12u },  // ISG1, legacy
		{ 24u, 0u, 4u, 12u },  // ISGN, fxc
};

inline uint32_t ReadU32(const uint8_t* p)
{
		uint32_t v;
		std::memcpy(&v, p, sizeof(v));
		return v;
}

// A semantic name must be NUL-terminated inside the chunk and start with a
// character that is legal for HLSL semantics. This is what lets us tell the
// candidate layouts apart.
inline bool IsPlausibleSemanticName(const uint8_t* chunkData, uint32_t chunkSize, uint32_t nameOffset)
{
		if (nameOffset >= chunkSize) return false;

		const char* name = reinterpret_cast<const char*>(chunkData) + nameOffset;
		const uint32_t maxLen = chunkSize - nameOffset;

		uint32_t len = 0;
		while (len < maxLen && name[len] != '\0') ++len;
		if (len == 0 || len == maxLen) return false;  // empty or unterminated

		const char c = name[0];
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline bool LayoutFits(const uint8_t* chunkData, uint32_t chunkSize,
											 uint32_t numElements, const SignatureElementLayout& layout)
{
		const uint64_t needed = 8ull + static_cast<uint64_t>(numElements) * layout.stride;
		if (needed > chunkSize) return false;

		const uint8_t* elements = chunkData + 8;
		for (uint32_t i = 0; i < numElements; ++i)
		{
				const uint8_t* elem = elements + static_cast<size_t>(i) * layout.stride;
				if (!IsPlausibleSemanticName(chunkData, chunkSize, ReadU32(elem + layout.nameOffsetField)))
						return false;
		}
		return true;
}

// Searches the input signature for `semanticName` at `semanticIndex`.
// When requireUint32 is true the element's component type must be UINT32 —
// used by the D3D12 backend to keep the strict MDI_INSTANCE_ID_PARAMETER
// match. fxc does not reliably report uint inputs, so the D3D11 backend
// passes false.
inline bool InputSignatureHasSemantic(const void* bytecode, size_t size,
																			const char* semanticName, uint32_t semanticIndex,
																			bool requireUint32)
{
		if (!bytecode || !semanticName || size < 32) return false;

		auto* data = static_cast<const uint8_t*>(bytecode);
		if (data[0] != 'D' || data[1] != 'X' || data[2] != 'B' || data[3] != 'C')
				return false;

		const uint32_t totalSize  = ReadU32(data + 24);
		const uint32_t chunkCount = ReadU32(data + 28);
		if (totalSize > size || chunkCount == 0 || chunkCount > 64) return false;
		if (32ull + static_cast<uint64_t>(chunkCount) * 4ull > size) return false;

		const uint8_t* chunkOffsets = data + 32;

		// ISGN = 'N','G','S','I' little-endian; ISG1 = '1','G','S','I'
		constexpr uint32_t kISGN = 0x4E475349u;
		constexpr uint32_t kISG1 = 0x31475349u;

		const size_t nameLen = std::strlen(semanticName);

		for (uint32_t i = 0; i < chunkCount; ++i)
		{
				const uint32_t off = ReadU32(chunkOffsets + static_cast<size_t>(i) * 4u);
				if (static_cast<uint64_t>(off) + 8ull > size) continue;

				const uint32_t fourCC    = ReadU32(data + off);
				const uint32_t chunkSize = ReadU32(data + off + 4);
				if (static_cast<uint64_t>(off) + 8ull + chunkSize > size) continue;
				if (fourCC != kISGN && fourCC != kISG1) continue;
				if (chunkSize < 8) continue;

				const uint8_t* chunkData = data + off + 8;

				const uint32_t numElements = ReadU32(chunkData);
				if (numElements == 0 || numElements > 64) continue;

				const SignatureElementLayout* layout = nullptr;
				for (const auto& candidate : kSignatureLayouts)
				{
						if (LayoutFits(chunkData, chunkSize, numElements, candidate))
						{
								layout = &candidate;
								break;
						}
				}
				if (!layout) break;  // signature chunk found but unparseable

				const uint8_t* elements = chunkData + 8;
				for (uint32_t j = 0; j < numElements; ++j)
				{
						const uint8_t* elem = elements + static_cast<size_t>(j) * layout->stride;

						const uint32_t nameOff = ReadU32(elem + layout->nameOffsetField);
						const uint32_t semIdx  = ReadU32(elem + layout->semanticIndexField);
						if (semIdx != semanticIndex) continue;

						const char* name = reinterpret_cast<const char*>(chunkData) + nameOff;
						const size_t maxNameLen = chunkSize - nameOff;
						if (maxNameLen < nameLen + 1) continue;
						if (std::strncmp(name, semanticName, nameLen + 1) != 0) continue;

						if (requireUint32 &&
								ReadU32(elem + layout->componentTypeField) != kComponentTypeUInt32)
								continue;

						return true;
				}

				// Found the signature chunk — no need to keep searching.
				break;
		}

		return false;
}

} // namespace MDIDxbc
