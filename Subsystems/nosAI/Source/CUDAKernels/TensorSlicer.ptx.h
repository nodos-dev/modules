#ifndef SLICETENSOR_INCLUDED
#define SLICETENSOR_INCLUDED
static const char SliceTensor[] = R"(//
// Generated by NVIDIA NVVM Compiler
//
// Compiler Build ID: CL-32965470
// Cuda compilation tools, release 12.2, V12.2.91
// Based on NVVM 7.0.1
//

.version 8.2
.target sm_52
.address_size 64

	// .globl	SliceTensor

.visible .entry SliceTensor(
	.param .u64 SliceTensor_param_0,
	.param .u32 SliceTensor_param_1,
	.param .u32 SliceTensor_param_2,
	.param .u32 SliceTensor_param_3,
	.param .u64 SliceTensor_param_4
)
{
	.reg .pred 	%p<8>;
	.reg .b16 	%rs<6>;
	.reg .b32 	%r<25>;
	.reg .b64 	%rd<24>;


	ld.param.u64 	%rd15, [SliceTensor_param_0];
	ld.param.u32 	%r12, [SliceTensor_param_1];
	ld.param.u32 	%r13, [SliceTensor_param_2];
	ld.param.u32 	%r11, [SliceTensor_param_3];
	ld.param.u64 	%rd16, [SliceTensor_param_4];
	cvta.to.global.u64 	%rd1, %rd16;
	cvta.to.global.u64 	%rd2, %rd15;
	mov.u32 	%r14, %ntid.x;
	mov.u32 	%r15, %ctaid.x;
	mov.u32 	%r16, %tid.x;
	mad.lo.s32 	%r17, %r15, %r14, %r16;
	mul.lo.s32 	%r1, %r17, %r13;
	setp.ge.s32 	%p1, %r1, %r12;
	setp.lt.s32 	%p2, %r11, 1;
	or.pred  	%p3, %p1, %p2;
	@%p3 bra 	$L__BB0_7;

	cvt.s64.s32 	%rd3, %r1;
	and.b32  	%r24, %r11, 3;
	add.s32 	%r19, %r11, -1;
	setp.lt.u32 	%p4, %r19, 3;
	mov.u32 	%r23, 0;
	@%p4 bra 	$L__BB0_4;

	sub.s32 	%r22, %r11, %r24;
	add.s64 	%rd17, %rd1, %rd3;
	add.s64 	%rd20, %rd17, 3;
	mov.u64 	%rd21, %rd2;

$L__BB0_3:
	ld.global.u8 	%rs1, [%rd21];
	st.global.u8 	[%rd20+-3], %rs1;
	ld.global.u8 	%rs2, [%rd21+1];
	st.global.u8 	[%rd20+-2], %rs2;
	ld.global.u8 	%rs3, [%rd21+2];
	st.global.u8 	[%rd20+-1], %rs3;
	ld.global.u8 	%rs4, [%rd21+3];
	st.global.u8 	[%rd20], %rs4;
	add.s32 	%r23, %r23, 4;
	add.s64 	%rd21, %rd21, 4;
	add.s64 	%rd20, %rd20, 4;
	add.s32 	%r22, %r22, -4;
	setp.ne.s32 	%p5, %r22, 0;
	@%p5 bra 	$L__BB0_3;

$L__BB0_4:
	setp.eq.s32 	%p6, %r24, 0;
	@%p6 bra 	$L__BB0_7;

	cvt.s64.s32 	%rd18, %r23;
	add.s64 	%rd19, %rd18, %rd3;
	add.s64 	%rd23, %rd1, %rd19;
	add.s64 	%rd22, %rd2, %rd18;

$L__BB0_6:
	.pragma "nounroll";
	ld.global.u8 	%rs5, [%rd22];
	st.global.u8 	[%rd23], %rs5;
	add.s64 	%rd23, %rd23, 1;
	add.s64 	%rd22, %rd22, 1;
	add.s32 	%r24, %r24, -1;
	setp.ne.s32 	%p7, %r24, 0;
	@%p7 bra 	$L__BB0_6;

$L__BB0_7:
	ret;

}

)";
#endif //SLICETENSOR_INCLUDED