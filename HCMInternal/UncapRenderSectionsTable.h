#pragma once
// ================================================================================================================
// GENERATED FILE - DO NOT EDIT BY HAND.
// Generator: Documents\Halo Mod And Tools\H2 Uncap Render Sections\hcm\gen_hcm_table.py
// Source:    Documents\Halo Mod And Tools\H2 Uncap Render Sections\spec.json (md5 5af8dd1a29ba58dc29f140f1ae40865d)
// Target:    halo2.dll 1.3528, stock md5 afd5c77177c04d050b0e6f1ad7ffb304; every row's stock bytes re-checked against it,
//            re-derived from the stock operand, matched to the spec's bake bytes and capstone-decoded for
//            four simulated HCM block placements (above and below the module).
// Re-run the generator after any spec.json revision, then rebuild HCMInternal.
// ================================================================================================================
#include <cstdint>
#include <cstddef>

namespace UncapRenderSectionsTable
{
	// How a row's new field value is computed. B = halo2.dll base, S = HCM block VA, rva/len = the row's instruction.
	enum class FieldKind : uint8_t
	{
		Rip,        // disp32 = (S + k) - (B + rva + len)      rip-relative lea into the block (k = block offset)
		ImageRva,   // disp32 = (S - B) + k                    image-base-relative SIB load (base reg holds __ImageBase)
		DataRel,    // disp32 = (B + k) - S                    sections-base-relative operand back to .data RVA k (stays)
		BlockConst, // disp32 = k                              sections-base-relative operand to block offset k
		ImmDiv4,    // imm32  = ((B + k) - S) / 4              add rcx,imm feeding lea [r15+rcx*4] -> .data RVA k
		Cap,        // imm32  = k                              the section cap (N); written LAST, restored FIRST
		Rel8,       // rel8   = k                              companion jge retarget
	};

	struct Row
	{
		const char* id;
		uint32_t    rva;       // instruction start
		uint8_t     len;       // instruction length
		uint8_t     fieldOff;  // offset of the rewritten field inside the instruction
		uint8_t     fieldLen;  // 4, or 1 for the rel8 companion
		FieldKind   kind;
		uint32_t    k;         // kind constant (see FieldKind)
		uint8_t     stock[16]; // stock instruction bytes (first len used)
	};

	inline constexpr uint32_t kN = 0x1000;                  // new section cap (4096)
	inline constexpr uint32_t kSlack = 32;                    // uncapped light-cache append (sub_1807ED7A0)
	inline constexpr uint32_t kEntries = 0x1020;            // E = N + slack (4128)
	inline constexpr uint32_t kStockCap = 850;                // stock cap / stock array length
	inline constexpr uint32_t kSectionStride = 48;
	inline constexpr uint32_t kBlockOffExt = 0x30600;       // extern_shader_override u8[E]
	inline constexpr uint32_t kBlockOffSii = 0x31620;       // shader_index_indices u8[E]
	inline constexpr uint32_t kBlockSize = 0x32640;         // 50 * E
	inline constexpr uint32_t kOldSectionsRva = 0x1662530;   // stock sections[850] (48 B)
	inline constexpr uint32_t kOldExtRva = 0x166C4A0;        // stock extern_shader_override[850]
	inline constexpr uint32_t kOldSiiRva = 0x166C7F2;        // stock shader_index_indices[850]
	inline constexpr uint32_t kCountRva = 0x166C490;         // s32 running section count (stays in .data)
	inline constexpr uint32_t kCameraCountRva = 0x166C494;   // s32 camera-pass count (stays in .data)
	inline constexpr uint32_t kCopyInEntries = 882;         // stock-addressable 850 + slack, copied in on apply
	inline constexpr uint32_t kCopyBackEntries = 850;       // ONLY 850 copied back (882 would clobber the counts)
	inline constexpr uint32_t kRevertCameraMax = 818;       // revert gate: camera count <= stock cap - slack
	inline constexpr uint32_t kAdoptLeaRva = 0x7ED887;        // lea r15,[sections]: S_cand = B + rva + 7 + disp32
	inline constexpr uint32_t kStockSizeOfImage = 0x2A38000; // a block inside [this, SizeOfImage) = baked dll

	inline constexpr Row kRows[] =
	{
		{ "URS_LEA_7ED7AD", 0x7ED7AD, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0x7C, 0x4D, 0xE7, 0x00 } }, // sub_1807ED7A0: lea rax, [rip + 0xe74d7c]
		{ "URS_LEA_7ED887", 0x7ED887, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x3D, 0xA2, 0x4C, 0xE7, 0x00 } }, // sub_1807ED850: lea r15, [rip + 0xe74ca2]
		{ "URS_LEA_7EDF84", 0x7EDF84, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x15, 0xA5, 0x45, 0xE7, 0x00 } }, // sub_1807EDF80: lea rdx, [rip + 0xe745a5]
		{ "URS_LEA_7EDFDB", 0x7EDFDB, 7, 3, 4, FieldKind::Rip, 0x28, { 0x48, 0x8D, 0x05, 0x76, 0x45, 0xE7, 0x00 } }, // sub_1807EDFD0: lea rax, [rip + 0xe74576]
		{ "URS_LEA_7EE625", 0x7EE625, 7, 3, 4, FieldKind::Rip, 0x18, { 0x48, 0x8D, 0x05, 0x1C, 0x3F, 0xE7, 0x00 } }, // sub_1807EE5F0: lea rax, [rip + 0xe73f1c]
		{ "URS_LEA_7EE908", 0x7EE908, 7, 3, 4, FieldKind::Rip, 0x18, { 0x48, 0x8D, 0x05, 0x39, 0x3C, 0xE7, 0x00 } }, // sub_1807EE5F0: lea rax, [rip + 0xe73c39]
		{ "URS_LEA_7F120A", 0x7F120A, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x35, 0x1F, 0x13, 0xE7, 0x00 } }, // sub_1807F11E0: lea r14, [rip + 0xe7131f]
		{ "URS_LEA_7F14C8", 0x7F14C8, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x35, 0x61, 0x10, 0xE7, 0x00 } }, // sub_1807F14A0: lea rsi, [rip + 0xe71061]
		{ "URS_LEA_7F1666", 0x7F1666, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x25, 0xC3, 0x0E, 0xE7, 0x00 } }, // sub_1807F1610: lea r12, [rip + 0xe70ec3]
		{ "URS_LEA_7F17B7", 0x7F17B7, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0x72, 0x0D, 0xE7, 0x00 } }, // sub_1807F1770: lea rax, [rip + 0xe70d72]
		{ "URS_LEA_7F1D70", 0x7F1D70, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0xB9, 0x07, 0xE7, 0x00 } }, // sub_1807F1D20: lea rax, [rip + 0xe707b9]
		{ "URS_LEA_7F217D", 0x7F217D, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x0D, 0xAC, 0x03, 0xE7, 0x00 } }, // sub_1807F2130: lea rcx, [rip + 0xe703ac]
		{ "URS_LEA_7F22E2", 0x7F22E2, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x0D, 0x47, 0x02, 0xE7, 0x00 } }, // sub_1807F2130: lea rcx, [rip + 0xe70247]
		{ "URS_LEA_7F26D0", 0x7F26D0, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x1D, 0x59, 0xFE, 0xE6, 0x00 } }, // sub_1807F26B0: lea r11, [rip + 0xe6fe59]
		{ "URS_LEA_7F274E", 0x7F274E, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x3D, 0xDB, 0xFD, 0xE6, 0x00 } }, // sub_1807F2740: lea rdi, [rip + 0xe6fddb]
		{ "URS_LEA_7F2949", 0x7F2949, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x1D, 0xE0, 0xFB, 0xE6, 0x00 } }, // sub_1807F2940: lea rbx, [rip + 0xe6fbe0]
		{ "URS_LEA_7F2AB1", 0x7F2AB1, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0x78, 0xFA, 0xE6, 0x00 } }, // sub_1807F2A90: lea rax, [rip + 0xe6fa78]
		{ "URS_LEA_7F2B3C", 0x7F2B3C, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x05, 0xED, 0xF9, 0xE6, 0x00 } }, // sub_1807F2A90: lea r8, [rip + 0xe6f9ed]
		{ "URS_LEA_7F2C24", 0x7F2C24, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x05, 0x05, 0xF9, 0xE6, 0x00 } }, // sub_1807F2A90: lea r8, [rip + 0xe6f905]
		{ "URS_LEA_7F2D14", 0x7F2D14, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0x15, 0xF8, 0xE6, 0x00 } }, // sub_1807F2A90: lea rax, [rip + 0xe6f815]
		{ "URS_LEA_7F2E8B", 0x7F2E8B, 7, 3, 4, FieldKind::Rip, 0x18, { 0x48, 0x8D, 0x05, 0xB6, 0xF6, 0xE6, 0x00 } }, // sub_1807F2E70: lea rax, [rip + 0xe6f6b6]
		{ "URS_LEA_7F5C6C", 0x7F5C6C, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x1D, 0xBD, 0xC8, 0xE6, 0x00 } }, // sub_1807F5BD0: lea rbx, [rip + 0xe6c8bd]
		{ "URS_LEA_7F5D68", 0x7F5D68, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0xC1, 0xC7, 0xE6, 0x00 } }, // sub_1807F5BD0: lea rax, [rip + 0xe6c7c1]
		{ "URS_LEA_7F5DD1", 0x7F5DD1, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x1D, 0x58, 0xC7, 0xE6, 0x00 } }, // sub_1807F5BD0: lea rbx, [rip + 0xe6c758]
		{ "URS_LEA_978113", 0x978113, 7, 3, 4, FieldKind::Rip, 0x0, { 0x48, 0x8D, 0x05, 0x16, 0xA4, 0xCE, 0x00 } }, // sub_1809780C0: lea rax, [rip + 0xcea416]
		{ "URS_LEA_97873D", 0x97873D, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x25, 0xEC, 0x9D, 0xCE, 0x00 } }, // sub_1809786A0: lea r12, [rip + 0xce9dec]
		{ "URS_LEA_978D63", 0x978D63, 7, 3, 4, FieldKind::Rip, 0x0, { 0x4C, 0x8D, 0x35, 0xC6, 0x97, 0xCE, 0x00 } }, // sub_1809786A0: lea r14, [rip + 0xce97c6]
		{ "URS_LEA_7EDFB3", 0x7EDFB3, 7, 3, 4, FieldKind::Rip, 0x30600, { 0x48, 0x8D, 0x0D, 0xE6, 0xE4, 0xE7, 0x00 } }, // sub_1807EDFB0: lea rcx, [rip + 0xe7e4e6]
		{ "URS_IBREL_7F239D", 0x7F239D, 8, 4, 4, FieldKind::ImageRva, 0x1C, { 0x43, 0x8B, 0x8C, 0xE7, 0x4C, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov ecx, dword ptr [r15 + r12*8 + 0x166254c]
		{ "URS_IBREL_7F23CA", 0x7F23CA, 8, 4, 4, FieldKind::ImageRva, 0x18, { 0x43, 0x8B, 0x8C, 0xE7, 0x48, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov ecx, dword ptr [r15 + r12*8 + 0x1662548]
		{ "URS_IBREL_7F2437", 0x7F2437, 9, 5, 4, FieldKind::ImageRva, 0x0, { 0x43, 0x0F, 0x10, 0x84, 0xE7, 0x30, 0x25, 0x66, 0x01 } }, // sub_1807F2320: movups xmm0, xmmword ptr [r15 + r12*8 + 0x1662530]
		{ "URS_IBREL_7F2444", 0x7F2444, 9, 5, 4, FieldKind::ImageRva, 0x10, { 0x43, 0x0F, 0x10, 0x8C, 0xE7, 0x40, 0x25, 0x66, 0x01 } }, // sub_1807F2320: movups xmm1, xmmword ptr [r15 + r12*8 + 0x1662540]
		{ "URS_IBREL_7F2461", 0x7F2461, 9, 5, 4, FieldKind::ImageRva, 0x20, { 0x43, 0x0F, 0x10, 0x84, 0xE7, 0x50, 0x25, 0x66, 0x01 } }, // sub_1807F2320: movups xmm0, xmmword ptr [r15 + r12*8 + 0x1662550]
		{ "URS_IBREL_7F249A", 0x7F249A, 8, 4, 4, FieldKind::ImageRva, 0x24, { 0x43, 0x8B, 0x84, 0xE7, 0x54, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov eax, dword ptr [r15 + r12*8 + 0x1662554]
		{ "URS_IBREL_7F24B9", 0x7F24B9, 9, 4, 4, FieldKind::ImageRva, 0x2A, { 0x43, 0x80, 0xBC, 0xE7, 0x5A, 0x25, 0x66, 0x01, 0x00 } }, // sub_1807F2320: cmp byte ptr [r15 + r12*8 + 0x166255a], 0
		{ "URS_IBREL_7F24CF", 0x7F24CF, 8, 4, 4, FieldKind::ImageRva, 0x24, { 0x47, 0x8B, 0xAC, 0xE7, 0x54, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov r13d, dword ptr [r15 + r12*8 + 0x1662554]
		{ "URS_IBREL_7F24ED", 0x7F24ED, 8, 4, 4, FieldKind::ImageRva, 0x18, { 0x43, 0x8B, 0x94, 0xE7, 0x48, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov edx, dword ptr [r15 + r12*8 + 0x1662548]
		{ "URS_IBREL_7F2543", 0x7F2543, 8, 4, 4, FieldKind::ImageRva, 0x8, { 0x4A, 0x8B, 0xB4, 0xE0, 0x38, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov rsi, qword ptr [rax + r12*8 + 0x1662538]
		{ "URS_IBREL_7F254B", 0x7F254B, 8, 4, 4, FieldKind::ImageRva, 0x18, { 0x42, 0x8B, 0x84, 0xE0, 0x48, 0x25, 0x66, 0x01 } }, // sub_1807F2320: mov eax, dword ptr [rax + r12*8 + 0x1662548]
		{ "URS_SREL_7ED997", 0x7ED997, 8, 4, 4, FieldKind::DataRel, 0x166CE68, { 0x41, 0x89, 0x94, 0x87, 0x38, 0xA9, 0x00, 0x00 } }, // sub_1807ED850: mov dword ptr [r15 + rax*4 + 0xa938], edx
		{ "URS_SREL_7ED9E3", 0x7ED9E3, 10, 6, 4, FieldKind::DataRel, 0x166CE48, { 0xF3, 0x41, 0x0F, 0x11, 0x84, 0x8F, 0x18, 0xA9, 0x00, 0x00 } }, // sub_1807ED850: movss dword ptr [r15 + rcx*4 + 0xa918], xmm0
		{ "URS_SREL_7ED9F2", 0x7ED9F2, 10, 6, 4, FieldKind::DataRel, 0x166CE4C, { 0xF2, 0x41, 0x0F, 0x11, 0x84, 0x8F, 0x1C, 0xA9, 0x00, 0x00 } }, // sub_1807ED850: movsd qword ptr [r15 + rcx*4 + 0xa91c], xmm0
		{ "URS_SREL_7EDA00", 0x7EDA00, 8, 4, 4, FieldKind::DataRel, 0x166CE54, { 0x41, 0x89, 0x84, 0x8F, 0x24, 0xA9, 0x00, 0x00 } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa924], eax
		{ "URS_SREL_7EDA08", 0x7EDA08, 8, 4, 4, FieldKind::DataRel, 0x166CE68, { 0x41, 0x89, 0x94, 0x8F, 0x38, 0xA9, 0x00, 0x00 } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa938], edx
		{ "URS_SREL_7EDA5C", 0x7EDA5C, 12, 4, 4, FieldKind::DataRel, 0x166CE58, { 0x41, 0xC7, 0x84, 0x8F, 0x28, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x7F } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa928], 0x7f7fffff
		{ "URS_SREL_7EDA68", 0x7EDA68, 12, 4, 4, FieldKind::DataRel, 0x166CE60, { 0x41, 0xC7, 0x84, 0x8F, 0x30, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x7F } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa930], 0x7f7fffff
		{ "URS_SREL_7EDA74", 0x7EDA74, 12, 4, 4, FieldKind::DataRel, 0x166CE5C, { 0x41, 0xC7, 0x84, 0x8F, 0x2C, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xFF } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa92c], 0xff7fffff
		{ "URS_SREL_7EDA80", 0x7EDA80, 12, 4, 4, FieldKind::DataRel, 0x166CE64, { 0x41, 0xC7, 0x84, 0x8F, 0x34, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xFF } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa934], 0xff7fffff
		{ "URS_SREL_7EDA8E", 0x7EDA8E, 12, 4, 4, FieldKind::DataRel, 0x166CE58, { 0x41, 0xC7, 0x84, 0x8F, 0x28, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xFF } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa928], 0xff7fffff
		{ "URS_SREL_7EDA9A", 0x7EDA9A, 12, 4, 4, FieldKind::DataRel, 0x166CE60, { 0x41, 0xC7, 0x84, 0x8F, 0x30, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xFF } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa930], 0xff7fffff
		{ "URS_SREL_7EDAA6", 0x7EDAA6, 12, 4, 4, FieldKind::DataRel, 0x166CE5C, { 0x41, 0xC7, 0x84, 0x8F, 0x2C, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x7F } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa92c], 0x7f7fffff
		{ "URS_SREL_7EDAB2", 0x7EDAB2, 12, 4, 4, FieldKind::DataRel, 0x166CE64, { 0x41, 0xC7, 0x84, 0x8F, 0x34, 0xA9, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x7F } }, // sub_1807ED850: mov dword ptr [r15 + rcx*4 + 0xa934], 0x7f7fffff
		{ "URS_SREL_7EDACC", 0x7EDACC, 9, 4, 4, FieldKind::BlockConst, 0x31620, { 0x41, 0xC6, 0x84, 0x3F, 0xC2, 0xA2, 0x00, 0x00, 0xFF } }, // sub_1807ED850: mov byte ptr [r15 + rdi + 0xa2c2], 0xff
		{ "URS_SREL_7EDAED", 0x7EDAED, 8, 4, 4, FieldKind::BlockConst, 0x31620, { 0x41, 0x88, 0x84, 0x3F, 0xC2, 0xA2, 0x00, 0x00 } }, // sub_1807ED850: mov byte ptr [r15 + rdi + 0xa2c2], al
		{ "URS_SREL_7EDAFC", 0x7EDAFC, 8, 4, 4, FieldKind::DataRel, 0x166CB44, { 0x41, 0x89, 0xB4, 0x87, 0x14, 0xA6, 0x00, 0x00 } }, // sub_1807ED850: mov dword ptr [r15 + rax*4 + 0xa614], esi
		{ "URS_SREL_7EDB37", 0x7EDB37, 8, 4, 4, FieldKind::BlockConst, 0x30600, { 0x41, 0x88, 0x84, 0x3F, 0x70, 0x9F, 0x00, 0x00 } }, // sub_1807ED850: mov byte ptr [r15 + rdi + 0x9f70], al
		{ "URS_SREL_7EDF8B", 0x7EDF8B, 8, 4, 4, FieldKind::BlockConst, 0x31620, { 0x0F, 0xB6, 0x8C, 0x10, 0xC2, 0xA2, 0x00, 0x00 } }, // sub_1807EDF80: movzx ecx, byte ptr [rax + rdx + 0xa2c2]
		{ "URS_SREL_7EDF9B", 0x7EDF9B, 7, 3, 4, FieldKind::DataRel, 0x166CB44, { 0x8B, 0x84, 0x82, 0x14, 0xA6, 0x00, 0x00 } }, // sub_1807EDF80: mov eax, dword ptr [rdx + rax*4 + 0xa614]
		{ "URS_IMM4_7EDA3E", 0x7EDA3E, 7, 3, 4, FieldKind::ImmDiv4, 0x166CE58, { 0x48, 0x81, 0xC1, 0x4A, 0x2A, 0x00, 0x00 } }, // sub_1807ED850: add rcx, 0x2a4a
		{ "URS_CAP_7ED871", 0x7ED871, 6, 2, 4, FieldKind::Cap, 0x1000, { 0x81, 0xFF, 0x52, 0x03, 0x00, 0x00 } }, // sub_1807ED850: cmp edi, 0x352
		{ "URS_COMP_SUBPOOL_GRACEFUL_709322", 0x709322, 2, 1, 1, FieldKind::Rel8, 0x10, { 0x7D, 0x12 } }, // sub_180709300: jge 0x709336 -> jge 0x709334
	};
	inline constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
	static_assert(kRowCount == 61, "row count changed - re-run gen_hcm_table.py");

	// Every 4 KB page that holds a rewritten field (VirtualProtect'ed OUTSIDE the suspend window).
	inline constexpr uint32_t kCodePages[] = { 0x709000, 0x7ED000, 0x7EE000, 0x7F1000, 0x7F2000, 0x7F5000, 0x978000 };
	inline constexpr size_t kCodePageCount = sizeof(kCodePages) / sizeof(kCodePages[0]);

	// Quiescence-gate hazard ranges (RVA, half-open): no suspended thread may have Rip, or any qword of its whole live
	// stack, inside any of these while fields are rewritten.
	struct Hazard { const char* func; uint32_t lo; uint32_t hi; };
	inline constexpr Hazard kHazards[] =
	{
		{ "sub_180709300", 0x709300, 0x709337 },
		{ "sub_1807ED7A0", 0x7ED7A0, 0x7ED7E5 },
		{ "sub_1807ED850", 0x7ED850, 0x7EDCD7 },
		{ "sub_1807EDF80", 0x7EDF80, 0x7EDFA9 },
		{ "sub_1807F5790", 0x7F5790, 0x7F598D },
	};
	inline constexpr size_t kHazardCount = sizeof(kHazards) / sizeof(kHazards[0]);
}
