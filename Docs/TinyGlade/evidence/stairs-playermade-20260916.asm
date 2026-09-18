; TinyGlade_楼梯逆向_附录D_玩家绘制楼梯.md 的反汇编证据（2026-09-16）
; 来源：D:/MyProject/Tiny Glade/tiny-glade.exe + tiny_glade.pdb，dumpbin /DISASM:BYTES /RANGE
; 浮点常量是 TG 米制单精度；'; ->0x...' 是 RIP 相对引用解引用后的内容（字符串 / f32 / f64）
; '; @0x...' 是 call/jmp 的目标地址。只保留正文引用到的片段。

; ==================================================================
; §1.3 determine_segment_type：slope = |dh|/|dxz|，>0.25 → 2，>2.5 → 3，否则 1
; in _ZN16system_decorator16decorator_visual6stairs22determine_segment_type17h3ba0ac42d88918dfE:
; range 0x14080C810-0x14080C88F
14080C810 sub rsp,28h
14080C814 test r9,r9
14080C817 je 000000014080C890
14080C819 lea rax,[r8+r9*4]
14080C81D add rax,0FFFFFFFFFFFFFFFCh
14080C821 je 000000014080C8A0
14080C823 test rdx,rdx
14080C826 je 000000014080C8AC
14080C82C lea rdx,[rcx+rdx*8]
14080C830 add rdx,0FFFFFFFFFFFFFFF8h
14080C834 je 000000014080C8BC
14080C83A movss xmm0,dword ptr [r8]
14080C83F subss xmm0,dword ptr [rax]
14080C843 andps xmm0,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]  ; ->0x14292e980 f32x4=(nan, nan, nan, nan)
14080C84A movsd xmm1,mmword ptr [rcx]
14080C84E movsd xmm2,mmword ptr [rdx]
14080C852 subps xmm1,xmm2
14080C855 mulps xmm1,xmm1
14080C858 movshdup xmm2,xmm1
14080C85C addss xmm2,xmm1
14080C860 xorps xmm1,xmm1
14080C863 sqrtss xmm1,xmm2
14080C867 divss xmm0,xmm1
14080C86B ucomiss xmm0,dword ptr [__real@3e800000]
14080C872 seta al
14080C875 inc al
14080C877 ucomiss xmm0,dword ptr [__real@40200000]
14080C87E movzx ecx,al
14080C881 mov eax,3
14080C886 cmovbe eax,ecx
14080C889 mov dl,1
14080C88B add rsp,28h
14080C88F ret

; ==================================================================
; §1.3 densely_sample_spline_segment：n = max(ceil(2L), 2)，自然三次样条段
; range 0x140809AF7-0x140809BF5
140809AF7 mov rax,qword ptr [rdx+8]
140809AFB cmp rax,r8
140809AFE jbe 0000000140809C4A
140809B04 mov rsi,rcx
140809B07 mov rcx,qword ptr [rdx]
140809B0A imul r9,r8,2Ch
140809B0E lea rdi,[rcx+r9]
140809B12 inc r8
140809B15 add rdx,14h
140809B19 cmp r8,rax
140809B1C lea rax,[rcx+r9+2Ch]
140809B21 cmovae rax,rdx
140809B25 movss xmm6,dword ptr [rax]
140809B29 subss xmm6,dword ptr [rcx+r9]
140809B2F movaps xmm0,xmm6
140809B32 addss xmm0,xmm6
140809B36 call ceilf  ; @0x142923E60
140809B3B cvttss2si rax,xmm0
140809B40 mov rcx,rax
140809B43 sar rcx,3Fh
140809B47 movaps xmm1,xmm0
140809B4A subss xmm1,dword ptr [__real@5f000000]
140809B52 cvttss2si rdx,xmm1
140809B57 and rdx,rcx
140809B5A or rdx,rax
140809B5D xor eax,eax
140809B5F xorps xmm1,xmm1
140809B62 ucomiss xmm0,xmm1
140809B65 cmovae rax,rdx
140809B69 ucomiss xmm0,dword ptr [__real@5f7fffff]
140809B70 mov rcx,0FFFFFFFFFFFFFFFFh
140809B77 cmovbe rcx,rax
140809B7B cmp rcx,3
140809B7F mov eax,2
140809B84 cmovae rax,rcx
140809B88 mov rcx,rax
140809B8B dec rcx
140809B8E js 0000000140809B9A
140809B90 xorps xmm0,xmm0
140809B93 cvtsi2ss xmm0,rcx
140809B98 jmp 0000000140809BB2
140809B9A mov rdx,rcx
140809B9D shr rdx,1
140809BA0 and ecx,1
140809BA3 or rcx,rdx
140809BA6 xorps xmm0,xmm0
140809BA9 cvtsi2ss xmm0,rcx
140809BAE addss xmm0,xmm0
140809BB2 movaps xmm1,xmm6
140809BB5 divss xmm1,xmm0
140809BB9 movss dword ptr [rbp-1Ch],xmm1
140809BBE mov qword ptr [rbp-48h],0
140809BC6 mov qword ptr [rbp-40h],rax
140809BCA lea rax,[rbp-1Ch]
140809BCE mov qword ptr [rbp-58h],rax
140809BD2 mov qword ptr [rbp-50h],rdi
140809BD6 lea r8,[anon.2e26c5de36f02c7e173690395399860c.6.llvm.4788573975063538604]  ; ->0x142c296c0 &str"/rustc/05f9846f893b09a1be1fc8560e33fc3c815cfecb\library\core\src\iter\traits\iterator.rs"
140809BDD lea rcx,[rbp-38h]
140809BE1 lea rdx,[rbp-58h]
140809BE5 call _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17haa2dcacd588bb942E  ; @0x141261550
140809BEA xorps xmm1,xmm1
140809BED mov rcx,rdi
140809BF0 call _ZN5utils13natural_cubic15SplineSegment2D18tangent_at_local_t17h8fafdc5f1299a652E  ; @0x140C92660
140809BF5 movaps xmm7,xmm0

; ==================================================================
; §1.1 derive_stair_node_params：DecoratorDst 跳表 → anchor 0/1/2
; range 0x14080C6A6-0x14080C7C2
14080C6A6 xor ebp,ebp
14080C6A8 movzx eax,byte ptr [rbx+22h]
14080C6AC lea ecx,[rax-2]
14080C6AF dec rax
14080C6B2 xor edx,edx
14080C6B4 cmp cl,3
14080C6B7 cmovb rdx,rax
14080C6BB lea rax,[142A8E448h]  ; ->0x142a8e448 f32x8=(-3.587e+37, -3.587e+37, -3.587e+37, -3.5871e+37, 7.1447e+31, 1.3901e+31, 7.2149e+22, 1.2946e+22)
14080C6C2 movsxd rcx,dword ptr [rax+rdx*4]
14080C6C6 add rcx,rax
14080C6C9 jmp rcx
14080C6CB xor ebp,ebp
14080C6CD movzx eax,byte ptr [rbx+22h]
14080C6D1 lea ecx,[rax-2]
14080C6D4 dec rax
14080C6D7 xor edx,edx
14080C6D9 cmp cl,3
14080C6DC cmovb rdx,rax
14080C6E0 lea rax,[142A8E448h]  ; ->0x142a8e448 f32x8=(-3.587e+37, -3.587e+37, -3.587e+37, -3.5871e+37, 7.1447e+31, 1.3901e+31, 7.2149e+22, 1.2946e+22)
14080C6E7 movsxd rcx,dword ptr [rax+rdx*4]
14080C6EB add rcx,rax
14080C6EE jmp rcx
14080C6F0 mov rcx,rbx
14080C6F3 call _ZN12country_core9resources5walls9decorator12DecoratorDst7as_wall17h05932608dfcddbfbE  ; @0x140833510
14080C6F8 mov r15,rax
14080C6FB mov rcx,rdi
14080C6FE call _ZN12country_core9resources5walls17decorator_storage20DecoratorStorageAddr7as_wall17h88262270acda9f9dE  ; @0x140A1C240
14080C703 lea r8,[142A8E360h]  ; ->0x142a8e360 &str"crates/systems/decorator/src/decorator_visual/stairs.rs"
14080C70A mov rcx,r14
14080C70D mov rdx,rax
14080C710 call _ZN12country_core9resources5walls12public_walls11PublicWalls3get17hf9b6b2a0178344aeE  ; @0x140A91EF0
14080C715 test rax,rax
14080C718 je 000000014080C7FA
14080C71E mov rcx,r15
14080C721 mov rdx,rax
14080C724 call _ZN5utils10wall_space14WallSpaceCoord11get_tangent17h651bf111d4d6fc73E  ; @0x140C924F0
14080C729 movaps xmm6,xmm0
14080C72C movaps xmm7,xmm1
14080C72F movaps xmm8,xmmword ptr [__xmm@80000000800000008000000080000000]  ; ->0x1429258f0 f32x4=(-0, -0, -0, -0)
14080C737 mov rcx,rbx
14080C73A call _ZN12country_core9resources5walls9decorator12DecoratorDst7as_wall17h05932608dfcddbfbE  ; @0x140833510
14080C73F cmp byte ptr [rax+22h],0
14080C743 jne 000000014080C784
14080C745 xorps xmm6,xmm8
14080C749 jmp 000000014080C788
14080C74B add rbx,30h
14080C74F lea rdx,[142A8E3A8h]  ; ->0x142a8e3a8 &str"crates/systems/decorator/src/decorator_visual/stairs.rs"
14080C756 mov rcx,rbx
14080C759 call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
14080C75E movss xmm1,dword ptr [rax]
14080C762 xor ecx,ecx
14080C764 jmp 000000014080C7AE
14080C766 add rbx,30h
14080C76A lea rdx,[142A8E390h]  ; ->0x142a8e390 &str"crates/systems/decorator/src/decorator_visual/stairs.rs"
14080C771 mov rcx,rbx
14080C774 call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
14080C779 movss xmm1,dword ptr [rax]
14080C77D mov ecx,1
14080C782 jmp 000000014080C7AE
14080C784 xorps xmm7,xmm8
14080C788 mov rcx,rdi
14080C78B call _ZN12country_core9resources5walls17decorator_storage20DecoratorStorageAddr7as_wall17h88262270acda9f9dE  ; @0x140A1C240
14080C790 mov rdi,rax
14080C793 movaps xmm0,xmm7
14080C796 movaps xmm1,xmm6
14080C799 call _ZN12country_core9resources5walls9decorator19DecoratorRotationXZ22from_xz_forward_vector17h96b6b6303869e27bE  ; @0x140833A00
14080C79E mov rcx,rdi
14080C7A1 movaps xmm1,xmm0
14080C7A4 call _ZN12country_core9resources6stairs22stairs_assembly_params15StairWallAnchor3new17he40adb17bc577a18E  ; @0x140959E80
14080C7A9 mov ecx,2
14080C7AE mov byte ptr [rsi+18h],bpl
14080C7B2 mov dword ptr [rsi],ecx
14080C7B4 movss dword ptr [rsi+4],xmm1
14080C7B9 mov qword ptr [rsi+8],rax
14080C7BD movss dword ptr [rsi+10h],xmm0
14080C7C2 mov rax,rsi

; ==================================================================
; §1.1 AlignKind::position_offset / outgoing_dir
; in _ZN12country_core9resources6stairs22stairs_assembly_params9AlignKind15position_offset17ha76d77b4c37fbe9dE:
; range 0x140959F00-0x140959F85
140959F00 movzx eax,byte ptr [rcx]
140959F03 xorps xmm0,xmm0
140959F06 test eax,eax
140959F08 je 0000000140959F2C
140959F0A cmp eax,1
140959F0D jne 0000000140959F44
140959F0F movsd xmm2,mmword ptr [rcx+4]
140959F14 cmp byte ptr [rcx+1],0
140959F18 je 0000000140959F22
140959F1A movsd xmm0,mmword ptr [rcx+0Ch]
140959F1F addps xmm2,xmm0
140959F22 mulss xmm1,dword ptr [__real@3f000000]
140959F2A jmp 0000000140959F3D
140959F2C cmp byte ptr [rcx+1],0
140959F30 jne 0000000140959F38
140959F32 cmp byte ptr [rcx+2],1
140959F36 jne 0000000140959F44
140959F38 movsd xmm2,mmword ptr [rcx+4]
140959F3D movsldup xmm0,xmm1
140959F41 mulps xmm0,xmm2
140959F44 movshdup xmm1,xmm0
140959F48 ret
140959F49 .......
140959F50 sub rsp,58h
140959F54 movzx eax,byte ptr [rcx]
140959F57 test eax,eax
140959F59 je 0000000140959F6C
140959F5B cmp eax,1
140959F5E jne 0000000140959F85
140959F60 mov eax,10h
140959F65 mov edx,0Ch
140959F6A jmp 0000000140959F76
140959F6C mov eax,8
140959F71 mov edx,4
140959F76 movss xmm0,dword ptr [rcx+rdx]
140959F7B movss xmm1,dword ptr [rcx+rax]
140959F80 add rsp,58h
140959F84 ret
140959F85 lea rax,[142AB4D28h]  ; ->0x142ab4d28 &str"AlignKind::None has no defined outgoing_dir"

; ==================================================================
; §1.1 StairWallAnchor::wall_normal = (-sin yaw, -cos yaw)
; in _ZN12country_core9resources6stairs22stairs_assembly_params15StairWallAnchor11wall_normal17h1e20e237088a288eE:
; range 0x140959E90-0x140959EE8
140959E90 sub rsp,58h
140959E94 movaps xmmword ptr [rsp+40h],xmm8
140959E9A movaps xmmword ptr [rsp+30h],xmm7
140959E9F movaps xmmword ptr [rsp+20h],xmm6
140959EA4 movss xmm6,dword ptr [rcx+8]
140959EA9 movaps xmm0,xmm6
140959EAC call sinf  ; @0x142923FD0
140959EB1 movaps xmm7,xmm0
140959EB4 movaps xmm8,xmmword ptr [__xmm@80000000800000008000000080000000]  ; ->0x1429258f0 f32x4=(-0, -0, -0, -0)
140959EBC xorps xmm7,xmm8
140959EC0 movaps xmm0,xmm6
140959EC3 call cosf  ; @0x142923E80
140959EC8 xorps xmm8,xmm0
140959ECC movaps xmm0,xmm7
140959ECF movaps xmm1,xmm8
140959ED3 movaps xmm6,xmmword ptr [rsp+20h]
140959ED8 movaps xmm7,xmmword ptr [rsp+30h]
140959EDD movaps xmm8,xmmword ptr [rsp+40h]
140959EE3 add rsp,58h
140959EE7 ret
140959EE8 ........

; ==================================================================
; §1.1 ui_change_stair_height：as_stair_mut()+0 = 高度偏移，Slider3d 范围 (0, 100) m
; range 0x1407ADFBC-0x1407AE141
1407ADFBC mov r14,rax
1407ADFBF movzx eax,byte ptr [rax+22h]
1407ADFC3 lea ecx,[rax-2]
1407ADFC6 dec rax
1407ADFC9 xor edx,edx
1407ADFCB cmp cl,3
1407ADFCE cmovb rdx,rax
1407ADFD2 lea rax,[142A832B0h]  ; ->0x142a832b0 f32x8=(-3.5006e+37, -3.5005e+37, -3.5005e+37, -3.5006e+37, -1.5, 11, 0, 0)
1407ADFD9 movsxd rcx,dword ptr [rax+rdx*4]
1407ADFDD add rcx,rax
1407ADFE0 jmp rcx
1407ADFE2 mov r15,qword ptr [r15]
1407ADFE5 lea rcx,[rsp+70h]
1407ADFEA call _ZN12country_core9resources5walls17decorator_storage20DecoratorStorageAddr7as_wall17h88262270acda9f9dE  ; @0x140A1C240
1407ADFEF lea r8,[142A83210h]  ; ->0x142a83210 &str"crates/systems/decorator/src/ui_systems/ui_change_stair_height.rs"
1407ADFF6 mov rcx,r15
1407ADFF9 mov rdx,rax
1407ADFFC call _ZN12country_core9resources5walls12public_walls11PublicWalls3get17hf9b6b2a0178344aeE  ; @0x140A91EF0
1407AE001 test rax,rax
1407AE004 je 00000001407AE287
1407AE00A mov rcx,rax
1407AE00D call _ZN12country_core9resources5walls17public_wall_state15PublicWallState11flat_roof_y17he6f49c9b65aa2c97E  ; @0x140B2A6E0
1407AE012 movaps xmm6,xmm0
1407AE015 mov rdx,qword ptr [r12]
1407AE019 lea r9,[142A83240h]  ; ->0x142a83240 &str"crates/systems/decorator/src/ui_systems/ui_change_stair_height.rs"
1407AE020 lea rcx,[rsp+0C0h]
1407AE028 mov r8d,edi
1407AE02B call _ZN12country_core9resources5walls27cached_decorator_transforms25CachedDecoratorTransforms8position17he331d10c11e7f55bE  ; @0x140A1A9F0
1407AE030 movss xmm0,dword ptr [rsp+0C0h]
1407AE039 movss xmm1,dword ptr [rsp+0C8h]
1407AE042 movss dword ptr [rsp+40h],xmm0
1407AE048 movss dword ptr [rsp+44h],xmm1
1407AE04E lea rcx,[rsp+0B4h]
1407AE056 lea rdx,[rsp+40h]
1407AE05B movaps xmm2,xmm6
1407AE05E jmp 00000001407AE0CD
1407AE060 mov rax,qword ptr [rsp+1D0h]
1407AE068 mov ecx,dword ptr [r14+8]
1407AE06C mov dword ptr [rsp+48h],ecx
1407AE070 mov rcx,qword ptr [r14]
1407AE073 mov qword ptr [rsp+40h],rcx
1407AE078 mov rax,qword ptr [rax]
1407AE07B movss xmm1,dword ptr [rsp+40h]
1407AE081 movss xmm2,dword ptr [rsp+44h]
1407AE087 mov rcx,qword ptr [rax+8]
1407AE08B mov rdx,qword ptr [rax+10h]
1407AE08F add rdx,rdx
1407AE092 mov qword ptr [rsp+0C0h],rcx
1407AE09A mov qword ptr [rsp+0C8h],rdx
1407AE0A2 movsd xmm0,mmword ptr [rax+50h]
1407AE0A7 movsd mmword ptr [rsp+0D0h],xmm0
1407AE0B0 lea rcx,[rsp+0C0h]
1407AE0B8 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407AE0BD lea rcx,[rsp+0B4h]
1407AE0C5 lea rdx,[rsp+40h]
1407AE0CA movaps xmm2,xmm0
1407AE0CD call _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E  ; @0x140C9CDE0
1407AE0D2 mov r15,qword ptr [rsp+1C0h]
1407AE0DA add r14,30h
1407AE0DE lea rdx,[142A83258h]  ; ->0x142a83258 &str"crates/systems/decorator/src/ui_systems/ui_change_stair_height.rs"
1407AE0E5 mov rcx,r14
1407AE0E8 call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
1407AE0ED movss xmm0,dword ptr [rax]
1407AE0F1 movss dword ptr [rsp+5Ch],xmm0
1407AE0F7 movsd xmm0,mmword ptr [__xmm@000000000000000042c8000000000000]  ; ->0x142a831a0 f64=5.27766e+13 u64=0x42c8000000000000
1407AE0FF movsd mmword ptr [rsp+0C0h],xmm0
1407AE108 mov byte ptr [rsp+0C8h],0
1407AE110 lea rax,[rsp+0C0h]
1407AE118 mov qword ptr [rsp+28h],rax
1407AE11D lea rax,[rsp+5Ch]
1407AE122 mov qword ptr [rsp+20h],rax
1407AE127 lea rcx,[rsp+40h]
1407AE12C lea r8,[rsp+60h]
1407AE131 lea r9,[rsp+0B4h]
1407AE139 mov rdx,r15
1407AE13C call _ZN12country_core7systems2ui9resources8slider3d8Slider3d6update17hc8c409c3293cb545E  ; @0x141276B20
1407AE141 test byte ptr [rsp+40h],1

; ==================================================================
; §2.3 ui_focus_stairs：宽度 Slider3d ±4 m，clamp(w + d, 0.45, 4.0)
; range 0x14079E0E1-0x14079E33F
14079E0E1 movsd xmm0,mmword ptr [__xmm@000000000000000040800000c0800000]  ; ->0x142a81c00 f64=512 u64=0x40800000c0800000
14079E0E9 movsd mmword ptr [rbp+580h],xmm0
14079E0F1 mov byte ptr [rbp+588h],0
14079E0F8 mov qword ptr [rsp+20h],rbx
14079E0FD lea rcx,[rbp+490h]
14079E104 lea r9,[rbp+370h]
14079E10B mov rdx,qword ptr [rbp+798h]
14079E112 mov r8,rsi
14079E115 call _ZN12country_core7systems2ui9resources8slider3d8Slider3d6update17h4e59486cf6f95fd6E  ; @0x141276990
14079E11A test byte ptr [rbp+490h],1
14079E121 je 000000014079EA39
14079E127 movss xmm7,dword ptr [rbp+4A0h]
14079E12F lea rcx,[rbp+4D0h]
14079E136 mov rdx,qword ptr [rbp+3B8h]
14079E13D mov r8d,edi
14079E140 call _ZN12country_core9resources6stairs12stairs_state11StairsState23get_all_connected_nodes17hb2f523e97ddd6991E  ; @0x1408FE8C0
14079E145 mov eax,dword ptr [rbp+38Ch]
14079E14B mov rcx,qword ptr [rbp+380h]
14079E152 mov dword ptr [rcx],eax
14079E154 mov rax,qword ptr [rbp+370h]
14079E15B mov dword ptr [rax],1
14079E161 movss dword ptr [rax+4],xmm7
14079E166 movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
14079E16D mov ecx,18h
14079E172 mov edx,4
14079E177 call __rust_alloc  ; @0x140613C10
14079E17C test rax,rax
14079E17F je 00000001407A1D21
14079E185 mov rcx,qword ptr [rbp+370h]
14079E18C movss xmm0,dword ptr [rcx+44h]
14079E191 movss xmm1,dword ptr [rcx+50h]
14079E196 mulss xmm1,dword ptr [__real@40800000]
14079E19E movaps xmm2,xmm0
14079E1A1 subss xmm2,xmm1
14079E1A5 addss xmm1,xmm0
14079E1A9 movsd xmm0,mmword ptr [rcx+48h]
14079E1AE mulps xmm0,xmmword ptr [__xmm@00000000000000004080000040800000]  ; ->0x142a81c10 f32x4=(4, 4, 0, 0)
14079E1B5 movsd xmm3,mmword ptr [rcx+3Ch]
14079E1BA movaps xmm4,xmm3
14079E1BD subps xmm4,xmm0
14079E1C0 movlps qword ptr [rax],xmm4
14079E1C3 movss dword ptr [rax+8],xmm2
14079E1C8 addps xmm0,xmm3
14079E1CB movlps qword ptr [rax+0Ch],xmm0
14079E1CF movss dword ptr [rax+14h],xmm1
14079E1D4 mov qword ptr [rbp+3C0h],2
14079E1DF mov qword ptr [rbp+3C8h],rax
14079E1E6 mov qword ptr [rbp+3D0h],2
14079E1F1 mov byte ptr [rbp+657h],1
14079E1F8 lea rcx,[rbp+580h]
14079E1FF lea rdx,[rbp+3C0h]
14079E206 call _ZN12country_core5utils9debug_viz9DebugLine3new17hfdc0a725d28fabfdE  ; @0x140B04E70
14079E20B mov byte ptr [rbp+657h],1
14079E212 lea rcx,[rbp+580h]
14079E219 mov rdx,qword ptr [rbp+568h]
14079E220 call _ZN12country_core5utils9debug_viz9DebugLine4show17h22c863dd57c39dc2E  ; @0x140B04F70
14079E225 mov rax,qword ptr [rbp+4D0h]
14079E22C mov qword ptr [rbp+648h],rax
14079E233 mov rax,qword ptr [rbp+4D8h]
14079E23A mov qword ptr [rbp+658h],rax
14079E241 mov rax,qword ptr [rbp+4E0h]
14079E248 test rax,rax
14079E24B je 000000014079E4B9
14079E251 mov rdi,qword ptr [rbp+658h]
14079E258 lea rsi,[rdi+rax*4]
14079E25C mov rax,qword ptr [rbp+790h]
14079E263 mov r12d,dword ptr [rax+1Ch]
14079E267 mov rbx,qword ptr [rax]
14079E26A mov r14,qword ptr [rax+10h]
14079E26E pcmpeqd xmm8,xmm8
14079E273 movss xmm9,dword ptr [__real@3ee66666]
14079E27C movss xmm10,dword ptr [__real@40800000]
14079E285 jmp 000000014079E450  ; @0x14079E450
14079E290 movdqu xmm1,xmmword ptr [rcx+r8]
14079E296 movdqa xmm2,xmm1
14079E29A pcmpeqb xmm2,xmm0
14079E29E pmovmskb r9d,xmm2
14079E2A3 test r9d,r9d
14079E2A6 je 000000014079E2E0
14079E2A8 tzcnt r10d,r9d
14079E2AD add r10,r8
14079E2B0 and r10,rdx
14079E2B3 lea r11,[r10*8]
14079E2BB mov r13,rcx
14079E2BE sub r13,r11
14079E2C1 cmp r15d,dword ptr [r13-8]
14079E2C5 je 000000014079E310
14079E2C7 lea r10d,[r9-1]
14079E2CB and r10w,r9w
14079E2CF mov r9d,r10d
14079E2D2 jne 000000014079E2A8
14079E2E0 pcmpeqb xmm1,xmm8
14079E2E5 pmovmskb r9d,xmm1
14079E2EA test r9d,r9d
14079E2ED jne 000000014079E4B0
14079E2F3 add r8,rax
14079E2F6 add r8,10h
14079E2FA add rax,10h
14079E2FE and r8,rdx
14079E301 jmp 000000014079E290
14079E310 neg r10
14079E313 movss xmm11,dword ptr [rcx+r10*8-4]
14079E31A mov rcx,qword ptr [rbp+638h]
14079E321 mov edx,r15d
14079E324 call _ZN12country_core9resources5walls17decorator_storage16DecoratorStorage14lookup_storage17h5399769f44acd8a5E  ; @0x140A1C3E0
14079E329 addss xmm11,xmm7
14079E32E movaps xmm0,xmm9
14079E332 maxss xmm0,xmm11
14079E337 movaps xmm1,xmm10
14079E33B minss xmm1,xmm0
14079E33F mov qword ptr [rbp+580h],rax

; ==================================================================
; §2.3 ui_focus_stairs：单节点宽度箭头 Slider3d 范围 (0.5, 4.0)
; range 0x1407A1A2A-0x1407A1A75
1407A1A2A movsd xmm0,mmword ptr [__xmm@0000000000000000408000003f000000]  ; ->0x142a81cb0 f64=512 u64=0x408000003f000000
1407A1A32 movsd mmword ptr [rbp+4D0h],xmm0
1407A1A3A mov byte ptr [rbp+4D8h],0
1407A1A41 mov byte ptr [rbp+677h],0
1407A1A48 lea rax,[rbp+4D0h]
1407A1A4F mov qword ptr [rsp+20h],rax
1407A1A54 lea rcx,[rbp+3C0h]
1407A1A5B mov rdx,qword ptr [rbp+798h]
1407A1A62 lea r8,[rbp+148h]
1407A1A69 lea r9,[rbp+580h]
1407A1A70 call _ZN12country_core7systems2ui9resources8slider3d8Slider3d6update17he3341ce50281949cE  ; @0x141276CA0
1407A1A75 test byte ptr [rbp+3C0h],1

; ==================================================================
; §5 set_decorator_color_id：+0x08 u32 / +0x10 u8 railing / +0x0C u32
; range 0x1422AF700-0x1422AF71A
1422AF700 mov rcx,rax
1422AF703 lea rdx,[14303CC00h]  ; ->0x14303cc00 &str"crates/systems/color/src/set_decorator_color_id.rs"
1422AF70A call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
1422AF70F add r15,4
1422AF713 mov dword ptr [rax+8],r14d
1422AF717 cmp r15,r13
1422AF71A jne 00000001422AF6E0

; ==================================================================
; §5 (续)
; range 0x1422AF920-0x1422AF93A
1422AF920 mov rcx,rax
1422AF923 lea rdx,[14303CBB8h]  ; ->0x14303cbb8 &str"crates/systems/color/src/set_decorator_color_id.rs"
1422AF92A call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
1422AF92F add r15,4
1422AF933 mov byte ptr [rax+10h],r14b
1422AF937 cmp r15,r13
1422AF93A jne 00000001422AF900

; ==================================================================
; §5 (续)
; range 0x1422AFB50-0x1422AFB6A
1422AFB50 mov rcx,rax
1422AFB53 lea rdx,[14303CB70h]  ; ->0x14303cb70 &str"crates/systems/color/src/set_decorator_color_id.rs"
1422AFB5A call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12as_stair_mut17hf2131e02ce5860edE  ; @0x1408394B0
1422AFB5F add r15,4
1422AFB63 mov dword ptr [rax+0Ch],r14d
1422AFB67 cmp r15,r13
1422AFB6A jne 00000001422AFB30

; ==================================================================
; §2.1 compute_stair_assembly_params：width[i] = lerp(w0, w1, t[i])
; range 0x140807270-0x140807347
140807270 movups xmm3,xmmword ptr [r15+rdx*4]
140807275 movups xmm4,xmmword ptr [r15+rdx*4+10h]
14080727B movaps xmm5,xmm2
14080727E subps xmm5,xmm3
140807281 movaps xmm8,xmm2
140807285 subps xmm8,xmm4
140807289 mulps xmm5,xmm0
14080728C mulps xmm8,xmm0
140807290 mulps xmm3,xmm1
140807293 addps xmm3,xmm5
140807296 mulps xmm4,xmm1
140807299 addps xmm4,xmm8
14080729D movups xmmword ptr [rax+rdx*4],xmm3
1408072A1 movups xmmword ptr [rax+rdx*4+10h],xmm4
1408072A6 add rdx,8
1408072AA cmp rcx,rdx
1408072AD jne 0000000140807270
1408072AF cmp r14,rcx
1408072B2 je 0000000140807347
1408072B8 mov rdx,rcx
1408072BB or rdx,1
1408072BF test r14b,1
1408072C3 je 00000001408072EB
1408072C5 movss xmm0,dword ptr [r15+rcx*4]
1408072CB movss xmm1,dword ptr [__real@3f800000]
1408072D3 subss xmm1,xmm0
1408072D7 mulss xmm1,xmm7
1408072DB mulss xmm0,xmm6
1408072DF addss xmm0,xmm1
1408072E3 movss dword ptr [rax+rcx*4],xmm0
1408072E8 mov rcx,rdx
1408072EB cmp r14,rdx
1408072EE je 0000000140807347
1408072F0 movss xmm0,dword ptr [__real@3f800000]
140807300 movss xmm1,dword ptr [r15+rcx*4]
140807306 movaps xmm2,xmm0
140807309 subss xmm2,xmm1
14080730D mulss xmm2,xmm7
140807311 mulss xmm1,xmm6
140807315 addss xmm1,xmm2
140807319 movss dword ptr [rax+rcx*4],xmm1
14080731E movss xmm1,dword ptr [r15+rcx*4+4]
140807325 movaps xmm2,xmm0
140807328 subss xmm2,xmm1
14080732C mulss xmm2,xmm7
140807330 mulss xmm1,xmm6
140807334 addss xmm1,xmm2
140807338 movss dword ptr [rax+rcx*4+4],xmm1
14080733E add rcx,2
140807342 cmp r14,rcx
140807345 jne 0000000140807300
140807347 mov qword ptr [rbp+1A8h],r14

; ==================================================================
; §2.1 heights fold 闭包：h = max(lerp(h0,h1,t), max(terrain(xz±perp*w/4)) - 0.1)
; range 0x141292F61-0x141293157
141292F61 mov rdi,qword ptr [rcx+88h]
141292F68 movaps xmm7,xmmword ptr [__xmm@80000000800000008000000080000000]  ; ->0x1429258f0 f32x4=(-0, -0, -0, -0)
141292F6F movss xmm8,dword ptr [__real@3f000000]
141292F78 movss xmm9,dword ptr [__real@bccccccd]
141292F81 movss xmm10,dword ptr [__real@3f800000]
141292F8A movss xmm11,dword ptr [__real@3dcccccd]
141292F93 xor esi,esi
141292F95 lea r13,[rbp+38h]
141292F99 jmp 0000000141292FE1
141292FA0 or r8,qword ptr [rax+rdx*8]
141292FA4 mov r9,qword ptr [rbp+70h]
141292FA8 inc rsi
141292FAB mov qword ptr [rax+rdx*8],r8
141292FAF movaps xmm0,xmm2
141292FB2 maxss xmm0,xmm1
141292FB6 cmpunordss xmm1,xmm1
141292FBB movaps xmm3,xmm1
141292FBE andps xmm3,xmm2
141292FC1 andnps xmm1,xmm0
141292FC4 orps xmm1,xmm3
141292FC7 mov rax,qword ptr [rbp+30h]
141292FCB movss dword ptr [rax+r9*4],xmm1
141292FD1 inc r9
141292FD4 inc rdi
141292FD7 cmp rsi,qword ptr [rbp-10h]
141292FDB je 0000000141292EB1
141292FE1 mov qword ptr [rbp+70h],r9
141292FE5 mov rax,qword ptr [rbp-18h]
141292FE9 add rax,rsi
141292FEC mov rcx,qword ptr [rbp]
141292FF0 lea rcx,[rax+rcx]
141292FF4 mov rdx,qword ptr [rbp-8]
141292FF8 lea rbx,[rcx+rdx]
141292FFC mov r8,qword ptr [rbp+10h]
141293000 movss xmm12,dword ptr [r8+rcx*8+4]
141293007 xorps xmm12,xmm7
14129300B mov rdx,qword ptr [rbp+8]
14129300F movss xmm13,dword ptr [rdx+rax*4]
141293015 mulss xmm13,xmm8
14129301A mulss xmm13,xmm8
14129301F mulss xmm12,xmm13
141293024 mulss xmm13,dword ptr [r8+rcx*8]
14129302A mov rax,qword ptr [r15]
14129302D movss xmm1,dword ptr [r14+rbx*8]
141293033 addss xmm1,xmm12
141293038 movss xmm2,dword ptr [r14+rbx*8+4]
14129303F addss xmm2,xmm13
141293044 mov rcx,qword ptr [rax+8]
141293048 mov rdx,qword ptr [rax+10h]
14129304C add rdx,rdx
14129304F mov qword ptr [rbp+38h],rcx
141293053 mov qword ptr [rbp+40h],rdx
141293057 movsd xmm0,mmword ptr [rax+50h]
14129305C movsd mmword ptr [rbp+48h],xmm0
141293061 mov rcx,r13
141293064 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
141293069 movaps xmm6,xmm0
14129306C mov rax,qword ptr [r15]
14129306F movss xmm1,dword ptr [r14+rbx*8]
141293075 movss xmm2,dword ptr [r14+rbx*8+4]
14129307C subss xmm1,xmm12
141293081 subss xmm2,xmm13
141293086 mov rcx,qword ptr [rax+8]
14129308A mov rdx,qword ptr [rax+10h]
14129308E add rdx,rdx
141293091 mov qword ptr [rbp+38h],rcx
141293095 mov qword ptr [rbp+40h],rdx
141293099 movsd xmm0,mmword ptr [rax+50h]
14129309E movsd mmword ptr [rbp+48h],xmm0
1412930A3 mov rcx,r13
1412930A6 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1412930AB mov rax,qword ptr [rbp+18h]
1412930AF movss xmm1,dword ptr [rax+rbx*4]
1412930B4 movaps xmm2,xmm10
1412930B8 subss xmm2,xmm1
1412930BC mov rax,qword ptr [rbp+20h]
1412930C0 mulss xmm2,dword ptr [rax]
1412930C4 mov rax,qword ptr [rbp+28h]
1412930C8 mulss xmm1,dword ptr [rax]
1412930CC mov rax,qword ptr [r12]
1412930D0 mov rcx,qword ptr [r12+8]
1412930D5 mov rdx,rcx
1412930D8 shr rdx,3
1412930DC mov qword ptr [rbp-20h],rdi
1412930E0 mov qword ptr [rbp-30h],0
1412930E8 mov qword ptr [rbp-28h],rdx
1412930EC cmp rdx,rdi
1412930EF jbe 0000000141293157
1412930F1 addss xmm6,xmm9
1412930F6 addss xmm0,xmm9
1412930FB addss xmm1,xmm2
1412930FF movaps xmm2,xmm6
141293102 cmpunordss xmm2,xmm6
141293107 movaps xmm3,xmm2
14129310A andps xmm3,xmm0
14129310D maxss xmm0,xmm6
141293111 andnps xmm2,xmm0
141293114 orps xmm2,xmm3
141293117 movaps xmm0,xmm2
14129311A addss xmm0,xmm11
14129311F mov edx,eax
141293121 and edx,7
141293124 and ecx,7
141293127 lea rcx,[rcx+rdx*8]
14129312B add rcx,rdi
14129312E mov rdx,rcx
141293131 mov r8d,1
141293137 shl r8,cl
14129313A shr rdx,6
14129313E and rax,0FFFFFFFFFFFFFFF8h
141293142 ucomiss xmm0,xmm1
141293145 ja 0000000141292FA0
14129314B not r8
14129314E and r8,qword ptr [rax+rdx*8]
141293152 jmp 0000000141292FA4  ; @0x141292FA4
141293157 lea rax,[rbp-28h]

; ==================================================================
; §2.1 Curve::try_new_from_points（Vec3 3D 累计长度）
; range 0x1401536D2-0x14015372D
1401536D2 mov rbx,qword ptr [r12+8]
1401536D7 movsd xmm2,mmword ptr [rbx]
1401536DB lea rcx,[rbx+8]
1401536DF movss xmm1,dword ptr [rbx+8]
1401536E4 xorps xmm0,xmm0
1401536E7 xor edx,edx
1401536F0 movss xmm3,dword ptr [rcx]
1401536F4 subss xmm1,xmm3
1401536F8 movsd xmm4,mmword ptr [rcx-8]
1401536FD subps xmm2,xmm4
140153700 mulps xmm2,xmm2
140153703 movshdup xmm5,xmm2
140153707 addss xmm5,xmm2
14015370B mulss xmm1,xmm1
14015370F addss xmm1,xmm5
140153713 sqrtss xmm1,xmm1
140153717 addss xmm0,xmm1
14015371B movss dword ptr [rax+rdx*4],xmm0
140153720 inc rdx
140153723 add rcx,0Ch
140153727 movaps xmm1,xmm3
14015372A movaps xmm2,xmm4
14015372D cmp r15,rdx

; ==================================================================
; §2.1 Curve::try_resample：N = round(len/spacing)，点数 N+1
; range 0x140152B65-0x140152C04
140152B65 mov r13,qword ptr [rdx+10h]
140152B69 cmp r13,1
140152B6D jbe 00000001401531DC
140152B73 xorps xmm1,xmm1
140152B76 ucomiss xmm2,xmm1
140152B79 jne 0000000140152B81
140152B7B jnp 00000001401531F4
140152B81 mov rbx,rdx
140152B84 movss xmm0,dword ptr [rdx+30h]
140152B89 ucomiss xmm0,xmm1
140152B8C jne 0000000140152B94
140152B8E jnp 000000014015320C
140152B94 mov rsi,rcx
140152B97 mov dword ptr [rbp+10h],r9d
140152B9B maxss xmm0,dword ptr [__real@322bcc77]
140152BA3 divss xmm2,xmm0
140152BA7 movss xmm0,dword ptr [__real@3f800000]
140152BAF minss xmm2,xmm0
140152BB3 divss xmm0,xmm2
140152BB7 call roundf  ; @0x142923FA0
140152BBC cvttss2si rax,xmm0
140152BC1 mov rcx,rax
140152BC4 sar rcx,3Fh
140152BC8 movaps xmm1,xmm0
140152BCB subss xmm1,dword ptr [__real@5f000000]
140152BD3 cvttss2si rdx,xmm1
140152BD8 and rdx,rcx
140152BDB or rdx,rax
140152BDE xor eax,eax
140152BE0 xorps xmm1,xmm1
140152BE3 ucomiss xmm0,xmm1
140152BE6 cmovae rax,rdx
140152BEA ucomiss xmm0,dword ptr [__real@5f7fffff]
140152BF1 mov r14,0FFFFFFFFFFFFFFFFh
140152BF8 cmovbe r14,rax
140152BFC cmp r14,1
140152C00 adc r14,1
140152C04 cmp r14,1

; ==================================================================
; §2.1 playermade_stairs_assemble：读节点 railing 字节(+0x10) → [rbp+6A8h]
; range 0x14001876F-0x1400187C1
14001876F add rax,30h
140018773 mov rcx,rax
140018776 call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12try_as_stair17hf110ffd52c735321E  ; @0x140839490
14001877B test rax,rax
14001877E je 0000000140018789
140018780 mov eax,dword ptr [rax+8]
140018783 mov dword ptr [rbp+640h],eax
140018789 mov rax,qword ptr [rbp+958h]
140018790 mov rcx,qword ptr [rax]
140018793 mov rax,qword ptr [rbp+670h]
14001879A mov edx,dword ptr [rax]
14001879C call _ZN12country_core9resources5walls17decorator_storage16DecoratorStorage21lookup_decorator_info17h171430a13fa3c740E  ; @0x140A1C930
1400187A1 test rax,rax
1400187A4 je 00000001400187E8
1400187A6 add rax,30h
1400187AA mov rcx,rax
1400187AD call _ZN12country_core9resources5walls9decorator18DecoratorExtraInfo12try_as_stair17hf110ffd52c735321E  ; @0x140839490
1400187B2 test rax,rax
1400187B5 je 00000001400187E8
1400187B7 movzx eax,byte ptr [rax+10h]
1400187BB mov dword ptr [rbp+6A8h],eax
1400187C1 xor r14d,r14d

; ==================================================================
; §2.1 playermade_stairs_assemble：逐分段 Curve<Vec3> + try_resample(0.5)
; range 0x14001A036-0x14001A0AD
14001A036 lea rdx,[rbp+6C0h]
14001A03D xor r8d,r8d
14001A040 call _ZN5utils5curve21Curve$LT$T$C$CSem$GT$19try_new_from_points17h0bff771c5cccd2e7E  ; @0x140153640
14001A045 xor esi,esi
14001A047 cmp rsi,qword ptr [rbp+590h]
14001A04E jo 000000014001C469
14001A054 mov rax,qword ptr [rbp+5C0h]
14001A05B mov qword ptr [rbp+6F0h],rax
14001A062 movups xmm0,xmmword ptr [rbp+590h]
14001A069 movups xmm1,xmmword ptr [rbp+5A0h]
14001A070 movups xmm2,xmmword ptr [rbp+5B0h]
14001A077 movaps xmmword ptr [rbp+6E0h],xmm2
14001A07E movaps xmmword ptr [rbp+6D0h],xmm1
14001A085 movaps xmmword ptr [rbp+6C0h],xmm0
14001A08C lea rcx,[rbp+460h]
14001A093 lea rdx,[rbp+6C0h]
14001A09A movd xmm2,dword ptr [__real@3f000000]
14001A0A2 mov r9d,1
14001A0A8 call _ZN5utils5curve21Curve$LT$T$C$CSem$GT$12try_resample17h0aaf96deeeef7d6dE  ; @0x140152B00
14001A0AD mov rax,qword ptr [rbp+6C0h]

; ==================================================================
; §2.1 重采样点：宽/方向/g 线性插值，type<2 抬到 terrain+0.15
; range 0x1400197C1-0x140019992
1400197C1 lea rax,[r13*2]
1400197C9 add rax,r13
1400197CC mov rcx,qword ptr [rbp+7D8h]
1400197D3 mov r15d,dword ptr [rcx+rax*4]
1400197D7 movss xmm6,dword ptr [rcx+rax*4+4]
1400197DD mov esi,dword ptr [rcx+rax*4+8]
1400197E1 mov rax,qword ptr [rbp+790h]
1400197E8 movss xmm1,dword ptr [rax+r13*4]
1400197EE lea rcx,[rbp+380h]
1400197F5 call _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h799831fd4e5aa432E  ; @0x140153380
1400197FA mov r12d,eax
1400197FD mov rdx,qword ptr [rbp+808h]
140019804 cmp rdx,r12
140019807 jbe 00000001400206CC
14001980D lea rbx,[r12+1]
140019812 cmp rbx,rdx
140019815 jae 00000001400206A4
14001981B movaps xmm11,xmm0
14001981F mov rax,qword ptr [rbp+7A0h]
140019826 movsd xmm7,mmword ptr [rax+r12*8]
14001982C movsd xmm8,mmword ptr [rax+r12*8+8]
140019833 mov rax,qword ptr [rbp+6B0h]
14001983A movss xmm10,dword ptr [rax+r12*4]
140019840 movss xmm9,dword ptr [rax+r12*4+4]
140019847 cmp byte ptr [rbp+7D0h],2
14001984E jae 00000001400198B5
140019850 movd xmm1,r15d
140019855 movd xmm2,esi
140019859 mov rdx,qword ptr [rbp+240h]
140019860 mov rax,qword ptr [rdx+8]
140019864 mov rcx,qword ptr [rdx+10h]
140019868 add rcx,rcx
14001986B mov qword ptr [rbp+6C0h],rax
140019872 mov qword ptr [rbp+6C8h],rcx
140019879 movsd xmm0,mmword ptr [rdx+50h]
14001987E movsd mmword ptr [rbp+6D0h],xmm0
140019886 lea rcx,[rbp+6C0h]
14001988D call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
140019892 addss xmm0,dword ptr [__real@3e19999a]
14001989A movaps xmm1,xmm6
14001989D cmpunordss xmm1,xmm6
1400198A2 movaps xmm2,xmm1
1400198A5 andps xmm2,xmm0
1400198A8 maxss xmm0,xmm6
1400198AC andnps xmm1,xmm0
1400198AF orps xmm1,xmm2
1400198B2 movaps xmm6,xmm1
1400198B5 mov rdi,qword ptr [rbp+660h]
1400198BC cmp rdi,qword ptr [rbp+650h]
1400198C3 jne 00000001400198D8
1400198C5 lea rcx,[rbp+650h]
1400198CC lea rdx,[1429263A8h]  ; ->0x1429263a8 &str"/home/h3/code/pouncelight/country-slice-private/crates/systems/decorator/src/decorator_visual/stairs_assemble.rs"
1400198D3 call _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h04e775870bab868eE  ; @0x140650050
1400198D8 subss xmm9,xmm10
1400198DD mulss xmm9,xmm11
1400198E2 addss xmm9,xmm10
1400198E7 mov rax,qword ptr [rbp+658h]
1400198EE movss dword ptr [rax+rdi*4],xmm9
1400198F4 inc rdi
1400198F7 mov qword ptr [rbp+660h],rdi
1400198FE mov rdi,qword ptr [rbp+690h]
140019905 cmp rdi,qword ptr [rbp+680h]
14001990C jne 0000000140019921
14001990E lea rcx,[rbp+680h]
140019915 lea rdx,[1429263C0h]  ; ->0x1429263c0 &str"/home/h3/code/pouncelight/country-slice-private/crates/systems/decorator/src/decorator_visual/stairs_assemble.rs"
14001991C call _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h09b7af127c6b1b53E  ; @0x140B81F90
140019921 subps xmm8,xmm7
140019925 movsldup xmm0,xmm11
14001992A mulps xmm0,xmm8
14001992E addps xmm0,xmm7
140019931 mov rax,qword ptr [rbp+688h]
140019938 movlps qword ptr [rax+rdi*8],xmm0
14001993C inc rdi
14001993F mov qword ptr [rbp+690h],rdi
140019946 mov rdi,qword ptr [rbp+518h]
14001994D cmp rdi,qword ptr [rbp+508h]
140019954 jne 0000000140019969
140019956 lea rcx,[rbp+508h]
14001995D lea rdx,[1429263D8h]  ; ->0x1429263d8 &str"/home/h3/code/pouncelight/country-slice-private/crates/systems/decorator/src/decorator_visual/stairs_assemble.rs"
140019964 call _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h37cdb4781d371fe0E  ; @0x140B9F180
140019969 mov rax,qword ptr [rbp+510h]
140019970 lea rcx,[rdi+rdi*2]
140019974 mov dword ptr [rax+rcx*4],r15d
140019978 movss dword ptr [rax+rcx*4+4],xmm6
14001997E mov dword ptr [rax+rcx*4+8],esi
140019982 inc rdi
140019985 mov qword ptr [rbp+518h],rdi
14001998C mov rdx,qword ptr [rbp+7F0h]

; ==================================================================
; §5 railing != None 分支：gen_stair_floors → construct_stair_steps(narrow=1, half=1)
; range 0x14001A7D4-0x14001A8AE
14001A7D4 call _ZN16system_decorator16decorator_visual15stairs_assemble16gen_stair_floors17h9955144ed8b6b91fE  ; @0x1407DC170
14001A7D9 mov rax,qword ptr [rbp+7E0h]
14001A7E0 cmp byte ptr [rax+0E8h],0
14001A7E7 je 000000014001A939
14001A7ED xor eax,eax
14001A7EF mov r10,qword ptr [rbp+7F0h]
14001A7F6 movd xmm0,dword ptr [__real@3f2147ae]
14001A7FE test al,al
14001A800 jne 000000014001A80A
14001A802 movd xmm0,dword ptr [__real@3e99999a]
14001A80A mov rcx,qword ptr [rbp+570h]
14001A811 mov rdx,qword ptr [rbp+578h]
14001A818 mov r8,qword ptr [rbp+658h]
14001A81F mov r9,qword ptr [rbp+660h]
14001A826 movups xmm1,xmmword ptr [rbp+688h]
14001A82D mov rax,qword ptr [rbp+938h]
14001A834 mov rax,qword ptr [rax]
14001A837 mov byte ptr [rbp+816h],1
14001A83E mov byte ptr [rbp+815h],1
14001A845 mov qword ptr [rsp+78h],r12
14001A84A mov qword ptr [rsp+70h],r10
14001A84F mov r10,qword ptr [rbp+240h]
14001A856 mov qword ptr [rsp+68h],r10
14001A85B movd dword ptr [rsp+60h],xmm0
14001A861 mov r10d,dword ptr [rbp+6A8h]
14001A868 mov byte ptr [rsp+58h],r10b
14001A86D mov qword ptr [rsp+48h],rax
14001A872 mov rax,qword ptr [rbp+788h]
14001A879 mov dword ptr [rsp+40h],eax
14001A87D lea rax,[rbp+140h]
14001A884 mov qword ptr [rsp+38h],rax
14001A889 lea rax,[rbp+428h]
14001A890 mov qword ptr [rsp+30h],rax
14001A895 movups xmmword ptr [rsp+20h],xmm1
14001A89A mov byte ptr [rsp+80h],1
14001A8A2 mov word ptr [rsp+50h],0
14001A8A9 call _ZN16system_decorator16decorator_visual15stairs_assemble21construct_stair_steps17h86b323f60c0fedb6E  ; @0x1407DA0B0
14001A8AE cmp byte ptr [rbp+6A8h],0

; ==================================================================
; §2.2 主分支调用：flags(挂墙且宽<=1.5) / min_width 0.63|0.3 / half=0
; range 0x14001AD53-0x14001AF32
14001AD53 cmp byte ptr [r14+0E8h],0
14001AD5B je 000000014001AD69
14001AD5D mov dword ptr [rbp+7A0h],0
14001AD67 jmp 000000014001AD7A
14001AD69 cmp byte ptr [r14+108h],0
14001AD71 sete al
14001AD74 mov dword ptr [rbp+7A0h],eax
14001AD7A mov rax,qword ptr [rbp+570h]
14001AD81 mov qword ptr [rbp+808h],rax
14001AD88 mov rbx,qword ptr [rbp+578h]
14001AD8F mov r15,qword ptr [rbp+658h]
14001AD96 mov rdi,qword ptr [rbp+660h]
14001AD9D mov rsi,qword ptr [rbp+688h]
14001ADA4 mov r14,qword ptr [rbp+690h]
14001ADAB mov rax,qword ptr [rbp+938h]
14001ADB2 mov r13,qword ptr [rax]
14001ADB5 mov byte ptr [rbp+816h],1
14001ADBC mov byte ptr [rbp+815h],1
14001ADC3 mov rcx,qword ptr [rbp+2D8h]
14001ADCA call _ZN12country_core9resources6stairs22stairs_assembly_params15StairNodeAnchor11try_as_wall17h3f9a76b4eb9f0ab4E  ; @0x140959EF0
14001ADCF test rax,rax
14001ADD2 je 000000014001ADFF
14001ADD4 cmp qword ptr [rbp+578h],0
14001ADDC je 0000000140020DB4
14001ADE2 mov rax,qword ptr [rbp+570h]
14001ADE9 movss xmm0,dword ptr [__real@3fc00000]
14001ADF1 ucomiss xmm0,dword ptr [rax]
14001ADF4 setae al
14001ADF7 mov dword ptr [rbp+6B0h],eax
14001ADFD jmp 000000014001AE09
14001ADFF mov dword ptr [rbp+6B0h],0
14001AE09 mov byte ptr [rbp+816h],1
14001AE10 mov byte ptr [rbp+815h],1
14001AE17 mov rcx,qword ptr [rbp+118h]
14001AE1E call _ZN12country_core9resources6stairs22stairs_assembly_params15StairNodeAnchor11try_as_wall17h3f9a76b4eb9f0ab4E  ; @0x140959EF0
14001AE23 test rax,rax
14001AE26 je 000000014001AE7C
14001AE28 mov rax,qword ptr [rbp+578h]
14001AE2F test rax,rax
14001AE32 je 0000000140020DD7
14001AE38 mov rcx,qword ptr [rbp+570h]
14001AE3F lea rax,[rcx+rax*4]
14001AE43 add rax,0FFFFFFFFFFFFFFFCh
14001AE47 test rax,rax
14001AE4A je 0000000140020DD7
14001AE50 movd xmm0,dword ptr [__real@3f2147ae]
14001AE58 cmp byte ptr [rbp+7A0h],0
14001AE5F jne 000000014001AE69
14001AE61 movd xmm0,dword ptr [__real@3e99999a]
14001AE69 movss xmm1,dword ptr [__real@3fc00000]
14001AE71 ucomiss xmm1,dword ptr [rax]
14001AE74 jb 000000014001AE95
14001AE76 mov ax,100h
14001AE7A jmp 000000014001AE97
14001AE7C movd xmm0,dword ptr [__real@3f2147ae]
14001AE84 cmp byte ptr [rbp+7A0h],0
14001AE8B jne 000000014001AE95
14001AE8D movd xmm0,dword ptr [__real@3e99999a]
14001AE95 xor eax,eax
14001AE97 movzx ecx,byte ptr [rbp+6B0h]
14001AE9E movzx eax,ax
14001AEA1 or eax,ecx
14001AEA3 mov byte ptr [rbp+816h],1
14001AEAA mov byte ptr [rbp+815h],1
14001AEB1 mov word ptr [rsp+50h],ax
14001AEB6 mov qword ptr [rsp+48h],r13
14001AEBB mov qword ptr [rsp+28h],r14
14001AEC0 mov qword ptr [rsp+20h],rsi
14001AEC5 mov qword ptr [rsp+78h],r12
14001AECA mov rax,qword ptr [rbp+7F0h]
14001AED1 mov qword ptr [rsp+70h],rax
14001AED6 mov rax,qword ptr [rbp+240h]
14001AEDD mov qword ptr [rsp+68h],rax
14001AEE2 movd dword ptr [rsp+60h],xmm0
14001AEE8 mov eax,dword ptr [rbp+6A8h]
14001AEEE mov byte ptr [rsp+58h],al
14001AEF2 mov rax,qword ptr [rbp+788h]
14001AEF9 mov dword ptr [rsp+40h],eax
14001AEFD lea rax,[rbp+140h]
14001AF04 mov qword ptr [rsp+38h],rax
14001AF09 lea rax,[rbp+428h]
14001AF10 mov qword ptr [rsp+30h],rax
14001AF15 mov byte ptr [rsp+80h],0
14001AF1D mov r8,r15
14001AF20 mov r9,rdi
14001AF23 mov rcx,qword ptr [rbp+808h]
14001AF2A mov rdx,rbx
14001AF2D call _ZN16system_decorator16decorator_visual15stairs_assemble21construct_stair_steps17h86b323f60c0fedb6E  ; @0x1407DA0B0
14001AF32 movzx ebx,byte ptr [rbp+806h]

; ==================================================================
; §2.2 construct_stair_steps：首段、块高 max(dy,0.6)、高端加厚、terrain+0.15
; range 0x1407DA124-0x1407DA5F2
1407DA124 mov rax,qword ptr [rbp+478h]
1407DA12B cmp rax,1
1407DA12F je 00000001407DB2BF
1407DA135 test rax,rax
1407DA138 je 00000001407DB2D5
1407DA13E mov rsi,r9
1407DA141 mov rbx,r8
1407DA144 mov rdi,rdx
1407DA147 mov r14,rcx
1407DA14A mov r12,qword ptr [rbp+4C8h]
1407DA151 mov r15,qword ptr [rbp+4B8h]
1407DA158 movss xmm6,dword ptr [rbp+4B0h]
1407DA160 mov r13,qword ptr [rbp+470h]
1407DA167 movss xmm0,dword ptr [r13+8]
1407DA16D movss xmm1,dword ptr [r13]
1407DA173 subss xmm1,dword ptr [r13+0Ch]
1407DA179 subss xmm0,dword ptr [r13+14h]
1407DA17F movss xmm2,dword ptr [r13+4]
1407DA185 mulss xmm1,xmm1
1407DA189 mulss xmm0,xmm0
1407DA18D addss xmm0,xmm1
1407DA191 xorps xmm1,xmm1
1407DA194 sqrtss xmm1,xmm0
1407DA198 movss dword ptr [rbp+354h],xmm2
1407DA1A0 subss xmm2,dword ptr [r13+10h]
1407DA1A6 andps xmm2,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]  ; ->0x14292e980 f32x4=(nan, nan, nan, nan)
1407DA1AD movaps xmmword ptr [rbp+1E0h],xmm2
1407DA1B4 movss xmm0,dword ptr [__real@3f0f5c29]
1407DA1BC divss xmm0,xmm1
1407DA1C0 call floorf  ; @0x142923EE0
1407DA1C5 cvttss2si rax,xmm0
1407DA1CA mov rcx,rax
1407DA1CD sar rcx,3Fh
1407DA1D1 movaps xmm1,xmm0
1407DA1D4 subss xmm1,dword ptr [__real@5f000000]
1407DA1DC cvttss2si rdx,xmm1
1407DA1E1 and rdx,rcx
1407DA1E4 or rdx,rax
1407DA1E7 xor eax,eax
1407DA1E9 xorps xmm1,xmm1
1407DA1EC ucomiss xmm0,xmm1
1407DA1EF cmovb rdx,rax
1407DA1F3 ucomiss xmm0,dword ptr [__real@5f7fffff]
1407DA1FA mov rcx,0FFFFFFFFFFFFFFFFh
1407DA201 cmova rdx,rcx
1407DA205 mov qword ptr [rbp+258h],rdx
1407DA20C xorps xmm0,xmm0
1407DA20F cmp byte ptr [rbp+4A8h],0
1407DA216 je 00000001407DA220
1407DA218 movss xmm0,dword ptr [__real@3e800000]
1407DA220 mov r9,qword ptr [rbp+478h]
1407DA227 lea rcx,[r9+r9*2]
1407DA22B lea rdx,[rcx*4]
1407DA233 add rdx,r13
1407DA236 mov qword ptr [rbp-20h],r13
1407DA23A mov qword ptr [rbp+250h],rdx
1407DA241 mov qword ptr [rbp-18h],rdx
1407DA245 mov qword ptr [rbp-10h],r14
1407DA249 lea rdx,[r14+rdi*4]
1407DA24D cmp rdi,r9
1407DA250 cmovae rdi,r9
1407DA254 mov qword ptr [rbp-8],rdx
1407DA258 lea rdx,[rbx+rsi*8]
1407DA25C cmp rsi,rdi
1407DA25F cmovae rsi,rdi
1407DA263 mov qword ptr [rbp],0
1407DA26B mov qword ptr [rbp+8],rdi
1407DA26F mov qword ptr [rbp+10h],r9
1407DA273 mov qword ptr [rbp+18h],rbx
1407DA277 mov r10,qword ptr [rbp+4C0h]
1407DA27E lea r8,[r10+r12*4]
1407DA282 cmp r12,rsi
1407DA285 cmovae r12,rsi
1407DA289 mov qword ptr [rbp+20h],rdx
1407DA28D mov qword ptr [rbp+28h],0
1407DA295 mov qword ptr [rbp+30h],rsi
1407DA299 mov qword ptr [rbp+38h],rdi
1407DA29D mov qword ptr [rbp+40h],r10
1407DA2A1 mov qword ptr [rbp+48h],r8
1407DA2A5 mov qword ptr [rbp+50h],0
1407DA2AD mov qword ptr [rbp+58h],r12
1407DA2B1 mov qword ptr [rbp+60h],rsi
1407DA2B5 mov qword ptr [rbp+68h],0
1407DA2BD mov qword ptr [rbp+0A8h],0
1407DA2C8 lea rcx,[r13+rcx*4-0Ch]
1407DA2CD mov qword ptr [rbp+248h],rcx
1407DA2D4 mov rcx,qword ptr [rbp+258h]
1407DA2DB add rcx,2
1407DA2DF mov rdx,r9
1407DA2E2 sub rdx,rcx
1407DA2E5 cmovb rdx,rax
1407DA2E9 mov qword ptr [rbp+190h],rdx
1407DA2F0 add r9,0FFFFFFFFFFFFFFFEh
1407DA2F4 mov qword ptr [rbp+198h],r9
1407DA2FB mov rax,qword ptr [r15+8]
1407DA2FF mov qword ptr [rbp+180h],rax
1407DA306 mov rax,qword ptr [r15+10h]
1407DA30A add rax,rax
1407DA30D mov qword ptr [rbp+188h],rax
1407DA314 movsd xmm1,mmword ptr [r15+50h]
1407DA31A movaps xmmword ptr [rbp+0B0h],xmm1
1407DA321 movsldup xmm0,xmm0
1407DA325 movaps xmmword ptr [rbp+0C0h],xmm0
1407DA32C movsldup xmm0,xmm6
1407DA330 movaps xmmword ptr [rbp+0D0h],xmm0
1407DA337 lea r12,[rbp+2D0h]
1407DA33E movss xmm7,dword ptr [__real@3f000000]
1407DA346 jmp 00000001407DA358
1407DA350 movss xmm7,dword ptr [__real@3f000000]
1407DA358 mov rcx,r12
1407DA35B lea rdx,[rbp-20h]
1407DA35F call _ZN107_$LT$itertools..tuple_impl..TupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h855e6b2abec06949E  ; @0x14129D810
1407DA364 mov r14,qword ptr [rbp+2D0h]
1407DA36B test r14,r14
1407DA36E je 00000001407DB245
1407DA374 mov qword ptr [rbp+330h],r13
1407DA37B mov rcx,qword ptr [rbp+0A8h]
1407DA382 mov qword ptr [rbp+348h],rcx
1407DA389 lea rax,[rcx+1]
1407DA38D mov r15,qword ptr [rbp+2D8h]
1407DA394 mov r13,qword ptr [rbp+2E0h]
1407DA39B mov rdx,qword ptr [rbp+2E8h]
1407DA3A2 mov rdi,qword ptr [rbp+2F0h]
1407DA3A9 mov rbx,qword ptr [rbp+2F8h]
1407DA3B0 mov rsi,qword ptr [rbp+300h]
1407DA3B7 mov r8,qword ptr [rbp+308h]
1407DA3BE mov qword ptr [rbp+0A8h],rax
1407DA3C5 movss xmm2,dword ptr [rdi+4]
1407DA3CA movss xmm13,dword ptr [r14+4]
1407DA3D0 movaps xmm12,xmm2
1407DA3D4 subss xmm12,xmm13
1407DA3D9 andps xmm12,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]  ; ->0x14292e980 f32x4=(nan, nan, nan, nan)
1407DA3E1 maxss xmm12,dword ptr [__real@3f19999a]
1407DA3EA movzx ecx,word ptr [rbp+4A0h]
1407DA3F1 test ecx,100h
1407DA3F7 jne 00000001407DA440
1407DA3F9 cmp qword ptr [rbp+248h],0
1407DA401 je 00000001407DB2A7
1407DA407 mov rax,qword ptr [rbp+250h]
1407DA40E movss xmm0,dword ptr [rax-8]
1407DA413 ucomiss xmm0,dword ptr [rbp+354h]
1407DA41A jbe 00000001407DA490
1407DA41C mov rax,qword ptr [rbp+348h]
1407DA423 cmp rax,qword ptr [rbp+190h]
1407DA42A seta al
1407DA42D test cl,1
1407DA430 je 00000001407DA461
1407DA432 jmp 00000001407DA4B0
1407DA440 test cl,1
1407DA443 jne 00000001407DA4BB
1407DA445 cmp qword ptr [rbp+248h],0
1407DA44D je 00000001407DB2B3
1407DA453 mov rax,qword ptr [rbp+250h]
1407DA45A movss xmm0,dword ptr [rax-8]
1407DA45F xor eax,eax
1407DA461 movss xmm1,dword ptr [rbp+354h]
1407DA469 ucomiss xmm1,xmm0
1407DA46C jbe 00000001407DA4B0
1407DA46E movaps xmm0,xmmword ptr [rbp+1E0h]
1407DA475 mov rcx,qword ptr [rbp+348h]
1407DA47C cmp rcx,qword ptr [rbp+258h]
1407DA483 jae 00000001407DA4B0
1407DA485 jmp 00000001407DA4BE
1407DA490 test cl,1
1407DA493 jne 00000001407DA4BB
1407DA495 xor eax,eax
1407DA497 movss xmm1,dword ptr [rbp+354h]
1407DA49F ucomiss xmm1,xmm0
1407DA4A2 ja 00000001407DA46E
1407DA4B0 movaps xmm0,xmmword ptr [rbp+1E0h]
1407DA4B7 test al,al
1407DA4B9 jne 00000001407DA4BE
1407DA4BB xorps xmm0,xmm0
1407DA4BE addss xmm12,xmm0
1407DA4C3 cmp byte ptr [rbp+4D0h],0
1407DA4CA mov qword ptr [rbp+260h],rdx
1407DA4D1 mov qword ptr [rbp+270h],r8
1407DA4D8 je 00000001407DA4DF
1407DA4DA mulss xmm12,xmm7
1407DA4DF movaps xmm6,xmm13
1407DA4E3 cmpunordss xmm6,xmm13
1407DA4E9 movaps xmm0,xmm6
1407DA4EC andps xmm0,xmm2
1407DA4EF movaps xmmword ptr [rbp+140h],xmm2
1407DA4F6 movaps xmm1,xmm2
1407DA4F9 maxss xmm1,xmm13
1407DA4FE andnps xmm6,xmm1
1407DA501 orps xmm6,xmm0
1407DA504 movss xmm0,dword ptr [r14]
1407DA509 mulss xmm0,xmm7
1407DA50D movss xmm3,dword ptr [r14+8]
1407DA513 mulss xmm3,xmm7
1407DA517 movss xmm1,dword ptr [rdi]
1407DA51B mulss xmm1,xmm7
1407DA51F addss xmm1,xmm0
1407DA523 movss xmm2,dword ptr [rdi+8]
1407DA528 mulss xmm2,xmm7
1407DA52C addss xmm2,xmm3
1407DA530 mov rax,qword ptr [rbp+180h]
1407DA537 mov qword ptr [rbp+2D0h],rax
1407DA53E mov rax,qword ptr [rbp+188h]
1407DA545 mov qword ptr [rbp+2D8h],rax
1407DA54C movaps xmm0,xmmword ptr [rbp+0B0h]
1407DA553 movlps qword ptr [rbp+2E0h],xmm0
1407DA55A mov rcx,r12
1407DA55D call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407DA562 addss xmm0,dword ptr [__real@3e19999a]
1407DA56A movaps xmm8,xmm6
1407DA56E cmpunordss xmm8,xmm6
1407DA574 movaps xmm1,xmm8
1407DA578 andps xmm1,xmm0
1407DA57B maxss xmm0,xmm6
1407DA57F andnps xmm8,xmm0
1407DA583 orps xmm8,xmm1
1407DA587 movss xmm0,dword ptr [r14]
1407DA58C movss xmm1,dword ptr [r14+8]
1407DA592 movss dword ptr [rbp+2D0h],xmm0
1407DA59A movss dword ptr [rbp+2D4h],xmm1
1407DA5A2 lea rcx,[rbp+1FCh]
1407DA5A9 mov rdx,r12
1407DA5AC movaps xmm2,xmm8
1407DA5B0 call _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E  ; @0x140C9CDE0
1407DA5B5 movss xmm0,dword ptr [rdi]
1407DA5B9 movss xmm1,dword ptr [rdi+8]
1407DA5BE movss dword ptr [rbp+2D0h],xmm0
1407DA5C6 movss dword ptr [rbp+2D4h],xmm1
1407DA5CE lea rcx,[rbp+1F0h]
1407DA5D5 mov rdx,r12
1407DA5D8 movaps xmm2,xmm8
1407DA5DC call _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E  ; @0x140C9CDE0
1407DA5E1 movaps xmm14,xmm12
1407DA5E5 mulss xmm14,dword ptr [__real@bf000000]
1407DA5EE xorps xmm9,xmm9
1407DA5F2 movaps xmm15,xmm14

; ==================================================================
; §2.2 construct_stair_steps：横向切砖（0.66/1.2/0.46/lerp(0.7,1.3)）
; range 0x1407DA845-0x1407DAA3A
1407DA845 movaps xmm1,xmmword ptr [rbp+0D0h]
1407DA84C movaps xmm0,xmm1
1407DA84F maxps xmm0,xmm7
1407DA852 cmpunordps xmm7,xmm7
1407DA856 andps xmm1,xmm7
1407DA859 andnps xmm7,xmm0
1407DA85C orps xmm7,xmm1
1407DA85F movaps xmm0,xmm7
1407DA862 mulps xmm0,xmmword ptr [__xmm@00000000000000003f0000003f000000]  ; ->0x1429258e0 f32x4=(0.5, 0.5, 0, 0)
1407DA869 movshdup xmm8,xmm0
1407DA86E addps xmm8,xmm0
1407DA872 movss xmm0,dword ptr [__real@40000000]
1407DA87A movss xmm1,dword ptr [__real@40800000]
1407DA882 movaps xmm2,xmm8
1407DA886 call _ZN5utils12inverse_lerp17hdcf8664c50f3232bE  ; @0x140C904A0
1407DA88B xorps xmm1,xmm1
1407DA88E maxss xmm1,xmm0
1407DA892 minss xmm10,xmm1
1407DA897 ucomiss xmm8,xmm9
1407DA89B movaps xmmword ptr [rbp+130h],xmm13
1407DA8A3 jb 00000001407DA8D0
1407DA8A5 mov edx,1
1407DA8AA movss xmm0,dword ptr [__real@3f28f5c3]
1407DA8B2 ucomiss xmm0,xmm8
1407DA8B6 jbe 00000001407DA8D0
1407DA8B8 mov r14,qword ptr [rbp+480h]
1407DA8BF mov r13,qword ptr [rbp+330h]
1407DA8C6 jmp 00000001407DAA0B  ; @0x1407DAA0B
1407DA8D0 ucomiss xmm8,dword ptr [__real@3f28f5c3]
1407DA8D8 mov r14,qword ptr [rbp+480h]
1407DA8DF mov r13,qword ptr [rbp+330h]
1407DA8E6 jb 00000001407DA930
1407DA8E8 movss xmm0,dword ptr [__real@3f99999a]
1407DA8F0 ucomiss xmm0,xmm8
1407DA8F4 jbe 00000001407DA930
1407DA8F6 mov rcx,qword ptr [r14]
1407DA8F9 mov rax,0A0761D6478BD642Fh
1407DA903 add rcx,rax
1407DA906 mov qword ptr [r14],rcx
1407DA909 mov rax,rcx
1407DA90C mov rdx,0E7037ED1A0B428DBh
1407DA916 xor rax,rdx
1407DA919 mul rax,rcx
1407DA91C xor rdx,rax
1407DA91F shr rdx,3Fh
1407DA923 inc rdx
1407DA926 jmp 00000001407DAA0B  ; @0x1407DAA0B
1407DA930 movss xmm9,dword ptr [__real@3f800000]
1407DA939 movaps xmm0,xmm9
1407DA93D subss xmm0,xmm10
1407DA942 mulss xmm0,dword ptr [__real@3f333333]
1407DA94A movaps xmm13,xmm10
1407DA94E mulss xmm13,dword ptr [__real@3fa66666]
1407DA957 addss xmm13,xmm0
1407DA95C mov rcx,r14
1407DA95F call _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E  ; @0x140CAF380
1407DA964 xorps xmm1,xmm1
1407DA967 maxss xmm1,xmm0
1407DA96B movaps xmm0,xmm9
1407DA96F minss xmm0,xmm1
1407DA973 movaps xmm1,xmm0
1407DA976 mulss xmm1,xmm0
1407DA97A addss xmm0,xmm0
1407DA97E movss xmm9,dword ptr [__real@40400000]
1407DA987 subss xmm9,xmm0
1407DA98C mulss xmm9,xmm1
1407DA991 mulss xmm9,xmm13
1407DA996 addss xmm9,dword ptr [__real@3eeb851f]
1407DA99F movaps xmm0,xmm8
1407DA9A3 divss xmm0,xmm9
1407DA9A8 call ceilf  ; @0x142923E60
1407DA9AD cvttss2si rax,xmm0
1407DA9B2 mov rcx,rax
1407DA9B5 sar rcx,3Fh
1407DA9B9 movaps xmm1,xmm0
1407DA9BC subss xmm1,dword ptr [__real@5f000000]
1407DA9C4 cvttss2si rdx,xmm1
1407DA9C9 and rdx,rcx
1407DA9CC or rdx,rax
1407DA9CF ucomiss xmm0,dword ptr [__real@00000000]
1407DA9D6 mov eax,0
1407DA9DB cmovb rdx,rax
1407DA9DF ucomiss xmm0,dword ptr [__real@5f7fffff]
1407DA9E6 mov rax,0FFFFFFFFFFFFFFFFh
1407DA9ED cmova rdx,rax
1407DA9F1 cmp rdx,1
1407DA9F5 adc rdx,0
1407DA9F9 mulss xmm9,dword ptr [__real@3e99999a]
1407DAA02 maxss xmm9,dword ptr [__real@3ea3d70a]
1407DAA0B inc rdx
1407DAA0E divss xmm9,xmm8
1407DAA13 lea rax,[142A883A8h]  ; ->0x142a883a8 &str"crates/systems/decorator/src/decorator_visual/stairs_assemble.rs"
1407DAA1A mov qword ptr [rsp+20h],rax
1407DAA1F lea rcx,[rbp+230h]
1407DAA26 movaps xmm2,xmm9
1407DAA2A mov r9,r14
1407DAA2D call _ZN5utils13random_splits17h445a73628407a507E  ; @0x140C90350
1407DAA32 mov rcx,r14
1407DAA35 call _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E  ; @0x140CAF380
1407DAA3A movaps xmm9,xmm0

; ==================================================================
; §2.2 construct_stair_steps：g<0.1 跳过、磨损量 m
; range 0x1407DAA3E-0x1407DAAAD
1407DAA3E mov rax,qword ptr [rbp+260h]
1407DAA45 movss xmm0,dword ptr [rax]
1407DAA49 movss xmm1,dword ptr [__real@3f000000]
1407DAA51 mulss xmm0,xmm1
1407DAA55 mov rax,qword ptr [rbp+270h]
1407DAA5C movss xmm13,dword ptr [rax]
1407DAA61 mulss xmm13,xmm1
1407DAA66 addss xmm13,xmm0
1407DAA6B movss xmm0,dword ptr [__real@3dcccccd]
1407DAA73 ucomiss xmm0,xmm13
1407DAA77 jbe 00000001407DAAAD
1407DAA79 cmp byte ptr [rbp+4D0h],0
1407DAA80 je 00000001407DAAAD
1407DAA82 mov rdx,qword ptr [rbp+230h]
1407DAA89 test rdx,rdx
1407DAA8C je 00000001407DA350
1407DAA92 mov rcx,qword ptr [rbp+238h]
1407DAA99 shl rdx,2
1407DAA9D mov r8d,4
1407DAAA3 call __rust_dealloc  ; @0x140613C20
1407DAAA8 jmp 00000001407DA350  ; @0x1407DA350
1407DAAAD movaps xmm0,xmmword ptr [rbp+2A0h]

; ==================================================================
; §2.2 construct_stair_steps：磨损 m 的两档上限（0.8dy / lerp 0.15-0.3 / lerp 0.05-0.15）
; range 0x1407DAD8C-0x1407DAE82
1407DAD8C mulss xmm13,dword ptr [__real@3ed70a3d]
1407DAD95 movsd xmm0,mmword ptr [rbp+208h]
1407DAD9D movaps xmmword ptr [rbp+280h],xmm0
1407DADA4 mulss xmm5,dword ptr [__real@3f4ccccd]
1407DADAC movss xmm4,dword ptr [__real@3f800000]
1407DADB4 movaps xmm0,xmm4
1407DADB7 subss xmm0,xmm10
1407DADBC movaps xmm1,xmm0
1407DADBF movss xmm3,dword ptr [__real@3e19999a]
1407DADC7 mulss xmm1,xmm3
1407DADCB movaps xmm7,xmm10
1407DADCF mulss xmm7,dword ptr [__real@3e99999a]
1407DADD7 addss xmm7,xmm1
1407DADDB movaps xmm1,xmm5
1407DADDE minss xmm1,xmm7
1407DADE2 cmpunordss xmm7,xmm7
1407DADE7 movaps xmm2,xmm7
1407DADEA andnps xmm2,xmm1
1407DADED movss xmm1,dword ptr [rbp+210h]
1407DADF5 movss dword ptr [rbp+290h],xmm1
1407DADFD andps xmm7,xmm5
1407DAE00 orps xmm7,xmm2
1407DAE03 mulss xmm0,dword ptr [__real@3d4ccccd]
1407DAE0B mulss xmm10,xmm3
1407DAE10 addss xmm10,xmm0
1407DAE15 movaps xmm0,xmm5
1407DAE18 minss xmm0,xmm10
1407DAE1D cmpunordss xmm10,xmm10
1407DAE23 movaps xmm1,xmm10
1407DAE27 andnps xmm1,xmm0
1407DAE2A movsd xmm0,mmword ptr [rbp+214h]
1407DAE32 movaps xmmword ptr [rbp+2A0h],xmm0
1407DAE39 andps xmm10,xmm5
1407DAE3D movss xmm0,dword ptr [rbp+21Ch]
1407DAE45 movss dword ptr [rbp+2B0h],xmm0
1407DAE4D orps xmm10,xmm1
1407DAE51 movaps xmm0,xmm4
1407DAE54 subss xmm0,xmm9
1407DAE59 mulss xmm0,xmm10
1407DAE5E movsd xmm1,mmword ptr [rbp+220h]
1407DAE66 movaps xmmword ptr [rbp+2C0h],xmm1
1407DAE6D mulss xmm7,xmm9
1407DAE72 movss xmm1,dword ptr [rbp+228h]
1407DAE7A movss dword ptr [rbp+1A0h],xmm1
1407DAE82 addss xmm7,xmm0

; ==================================================================
; §2.2 construct_stair_steps：进深 ×1.13、写 96 字节墙砖记录 flags=0x60
; range 0x1407DAFC9-0x1407DB202
1407DAFC9 xorps xmm4,xmm4
1407DAFCC sqrtss xmm4,xmm6
1407DAFD0 mulss xmm4,dword ptr [__real@3f90a3d7]
1407DAFD8 mulss xmm4,xmm1
1407DAFDC movaps xmm5,xmmword ptr [__xmm@00000000000000003f0000003f000000]  ; ->0x1429258e0 f32x4=(0.5, 0.5, 0, 0)
1407DAFE3 mulps xmm3,xmm5
1407DAFE6 mulps xmm0,xmm5
1407DAFE9 addps xmm0,xmm3
1407DAFEC movlps qword ptr [rbp+170h],xmm0
1407DAFF3 movss dword ptr [rbp+178h],xmm2
1407DAFFB movss xmm6,dword ptr [r13]
1407DB001 movss xmm14,dword ptr [rsi]
1407DB006 movss xmm0,dword ptr [rbp+290h]
1407DB00E mulss xmm0,xmm4
1407DB012 movsldup xmm2,xmm4
1407DB016 mulps xmm2,xmmword ptr [rbp+280h]
1407DB01D movlps qword ptr [rbp+160h],xmm2
1407DB024 movaps xmm2,xmm12
1407DB028 mulss xmm2,xmm1
1407DB02C movss dword ptr [rbp+168h],xmm0
1407DB034 movss xmm0,dword ptr [rbp+2B0h]
1407DB03C mulss xmm0,xmm2
1407DB040 movsldup xmm2,xmm2
1407DB044 mulps xmm2,xmmword ptr [rbp+2A0h]
1407DB04B movlps qword ptr [rbp+150h],xmm2
1407DB052 movss dword ptr [rbp+158h],xmm0
1407DB05A movss xmm0,dword ptr [rbp+1A0h]
1407DB062 mulss xmm0,xmm1
1407DB066 movsldup xmm1,xmm1
1407DB06A mulps xmm1,xmmword ptr [rbp+2C0h]
1407DB071 movlps qword ptr [rbp+2D0h],xmm1
1407DB078 movss dword ptr [rbp+2D8h],xmm0
1407DB080 lea rax,[rbp+170h]
1407DB087 mov qword ptr [rsp+20h],rax
1407DB08C lea rcx,[rbp-50h]
1407DB090 lea rdx,[rbp+160h]
1407DB097 lea r8,[rbp+150h]
1407DB09E mov r9,r12
1407DB0A1 call _ZN12country_core9resources6render13Affine3Packed9from_cols17hcaf7038d8f275b8cE  ; @0x14089D500
1407DB0A6 movss xmm15,dword ptr [r13]
1407DB0AC mov rcx,r14
1407DB0AF call _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E  ; @0x140CAF380
1407DB0B4 movaps xmm9,xmm0
1407DB0B8 addss xmm15,dword ptr [__real@bf000000]
1407DB0C1 mulss xmm9,dword ptr [__real@3d4ccccd]
1407DB0CA movss xmm10,dword ptr [rsi]
1407DB0CF mov rcx,r14
1407DB0D2 call _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E  ; @0x140CAF380
1407DB0D7 movaps xmm8,xmm0
1407DB0DB mov ecx,dword ptr [rbp+490h]
1407DB0E1 call _ZN12country_core8geometry14instanced_wall13BrickSourceId12from_wall_id17h6bd75ff6f436fcd8E  ; @0x140858750
1407DB0E6 movss xmm3,dword ptr [__real@3f800000]
1407DB0EE movaps xmm0,xmm3
1407DB0F1 subss xmm0,xmm6
1407DB0F5 movaps xmm1,xmm3
1407DB0F8 subss xmm1,xmm14
1407DB0FD unpcklps xmm1,xmm6
1407DB100 shufps xmm1,xmm1,0E1h
1407DB104 movaps xmm2,xmm11
1407DB108 shufps xmm2,xmm11,0E1h
1407DB10D mulps xmm2,xmm1
1407DB110 unpcklps xmm0,xmm14
1407DB114 mulps xmm0,xmm11
1407DB118 addps xmm0,xmm2
1407DB11B movaps xmm2,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]  ; ->0x14292e980 f32x4=(nan, nan, nan, nan)
1407DB122 andps xmm15,xmm2
1407DB126 addss xmm15,xmm15
1407DB12B movaps xmm1,xmm3
1407DB12E subss xmm1,xmm15
1407DB133 mulss xmm1,xmm7
1407DB137 subss xmm9,xmm1
1407DB13C mulss xmm8,dword ptr [__real@3d4ccccd]
1407DB145 addss xmm10,dword ptr [__real@bf000000]
1407DB14E andps xmm10,xmm2
1407DB152 addss xmm10,xmm10
1407DB157 movaps xmm1,xmm3
1407DB15A subss xmm1,xmm10
1407DB15F mulss xmm1,xmm7
1407DB163 subss xmm8,xmm1
1407DB168 unpcklps xmm8,xmm9
1407DB16C mov rcx,qword ptr [rbp+348h]
1407DB173 lea ecx,[r15+rcx]
1407DB177 movups xmm1,xmmword ptr [rbp-50h]
1407DB17B movups xmm2,xmmword ptr [rbp-40h]
1407DB17F movups xmm3,xmmword ptr [rbp-30h]
1407DB183 movss xmm4,dword ptr [rsi]
1407DB187 movss xmm5,dword ptr [r13]
1407DB18D unpcklps xmm5,xmm4
1407DB190 mulps xmm5,xmmword ptr [rbp+1B0h]
1407DB197 movaps xmmword ptr [rbp+2F0h],xmm3
1407DB19E movaps xmmword ptr [rbp+2E0h],xmm2
1407DB1A5 movaps xmmword ptr [rbp+2D0h],xmm1
1407DB1AC movddup xmm1,xmm8
1407DB1B1 movapd xmm2,xmmword ptr [rbp+1C0h]
1407DB1B9 andnpd xmm2,xmm1
1407DB1BD movapd xmmword ptr [rbp+300h],xmm2
1407DB1C5 movlps qword ptr [rbp+310h],xmm0
1407DB1CC mov dword ptr [rbp+318h],0
1407DB1D6 mov dword ptr [rbp+31Ch],ecx
1407DB1DC mov dword ptr [rbp+320h],eax
1407DB1E2 movlps qword ptr [rbp+324h],xmm5
1407DB1E9 mov dword ptr [rbp+32Ch],60h
1407DB1F3 mov rcx,qword ptr [rbp+488h]
1407DB1FA mov rdx,r12
1407DB1FD call _ZN12country_core6render22sparse_instance_buffer35SparseInstanceBufferWriter$LT$T$GT$4push17h6631bf7514bf4af0E  ; @0x14128CDD0
1407DB202 inc r15d

; ==================================================================
; §6.4 utils::random_splits_into：0, k/(n-1) ± jitter, 1
; range 0x140C90085-0x140C9025D
140C90085 mov rsi,r9
140C90088 cmp rcx,3
140C9008C jae 0000000140C900BD
140C9008E mov rax,qword ptr [rsi]
140C90091 mov rdx,qword ptr [rsi+10h]
140C90095 sub rax,rdx
140C90098 cmp rax,1
140C9009C jbe 0000000140C9028A
140C900A2 mov rax,qword ptr [rsi+8]
140C900A6 add rsi,10h
140C900AA mov rcx,3F80000000000000h
140C900B4 mov qword ptr [rax+rdx*4],rcx
140C900B8 jmp 0000000140C9025D  ; @0x140C9025D
140C900BD mov rdi,r8
140C900C0 movaps xmm6,xmm1
140C900C3 mov rbx,rcx
140C900C6 mov rax,rcx
140C900C9 dec rax
140C900CC js 0000000140C900D5
140C900CE cvtsi2ss xmm0,rax
140C900D3 jmp 0000000140C900EA
140C900D5 mov rcx,rax
140C900D8 shr rcx,1
140C900DB and eax,1
140C900DE or rax,rcx
140C900E1 cvtsi2ss xmm0,rax
140C900E6 addss xmm0,xmm0
140C900EA mov r14,qword ptr [rbp+80h]
140C900F1 movss xmm7,dword ptr [__real@3f800000]
140C900F9 movaps xmm8,xmm7
140C900FD divss xmm8,xmm0
140C90102 mulss xmm8,dword ptr [__real@3efd70a4]
140C9010B movaps xmm9,xmm8
140C9010F minss xmm9,xmm6
140C90114 cmpunordss xmm6,xmm6
140C90119 mov r15,qword ptr [rsi+10h]
140C9011D cmp r15,qword ptr [rsi]
140C90120 jne 0000000140C9012D
140C90122 mov rcx,rsi
140C90125 mov rdx,r14
140C90128 call _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h081e4b9c7b1fd67bE  ; @0x140B5F870
140C9012D andps xmm8,xmm6
140C90131 andnps xmm6,xmm9
140C90135 mov rax,qword ptr [rsi+8]
140C90139 mov dword ptr [rax+r15*4],0
140C90141 inc r15
140C90144 mov qword ptr [rsi+10h],r15
140C90148 test rbx,rbx
140C9014B js 0000000140C90158
140C9014D xorps xmm9,xmm9
140C90151 cvtsi2ss xmm9,rbx
140C90156 jmp 0000000140C90174
140C90158 mov rax,rbx
140C9015B shr rax,1
140C9015E mov ecx,ebx
140C90160 and ecx,1
140C90163 or rcx,rax
140C90166 xorps xmm9,xmm9
140C9016A cvtsi2ss xmm9,rcx
140C9016F addss xmm9,xmm9
140C90174 orps xmm6,xmm8
140C90178 addss xmm9,dword ptr [__real@bf800000]
140C90181 add rbx,0FFFFFFFFFFFFFFFEh
140C90185 mov rax,qword ptr [rsi]
140C90188 sub rax,r15
140C9018B cmp rax,rbx
140C9018E jb 0000000140C902B0
140C90194 lea rax,[rsi+10h]
140C90198 mov qword ptr [rbp-48h],rax
140C9019C divss xmm7,xmm9
140C901A1 mov qword ptr [rbp-50h],r15
140C901A5 lea r15,[r15*4]
140C901AD mov r12,qword ptr [rsi+8]
140C901B1 add r12,r15
140C901B4 mov qword ptr [rbp-40h],0
140C901BC movss xmm8,dword ptr [__real@bf000000]
140C901C5 jmp 0000000140C901FC
140C901D0 xorps xmm1,xmm1
140C901D3 cvtsi2ss xmm1,r13
140C901D8 mulss xmm1,xmm7
140C901DC addss xmm0,xmm8
140C901E1 mulss xmm0,xmm6
140C901E5 addss xmm0,xmm1
140C901E9 mov rax,qword ptr [rbp-40h]
140C901ED movss dword ptr [r12+rax*4],xmm0
140C901F3 mov qword ptr [rbp-40h],r13
140C901F7 cmp rbx,r13
140C901FA je 0000000140C9022A
140C901FC mov rcx,rdi
140C901FF call _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E  ; @0x140CAF380
140C90204 mov r13,qword ptr [rbp-40h]
140C90208 inc r13
140C9020B jns 0000000140C901D0
140C9020D mov rax,r13
140C90210 shr rax,1
140C90213 mov ecx,r13d
140C90216 and ecx,1
140C90219 or rcx,rax
140C9021C xorps xmm1,xmm1
140C9021F cvtsi2ss xmm1,rcx
140C90224 addss xmm1,xmm1
140C90228 jmp 0000000140C901D8
140C9022A mov rdi,qword ptr [rbp-50h]
140C9022E lea rax,[rdi+r13]
140C90232 mov qword ptr [rsi+10h],rax
140C90236 cmp rax,qword ptr [rsi]
140C90239 jne 0000000140C90246
140C9023B mov rcx,rsi
140C9023E mov rdx,r14
140C90241 call _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h081e4b9c7b1fd67bE  ; @0x140B5F870
140C90246 lea rdx,[rdi+r13]
140C9024A dec rdx
140C9024D add r15,qword ptr [rsi+8]
140C90251 mov dword ptr [r15+r13*4],3F800000h
140C90259 mov rsi,qword ptr [rbp-48h]
140C9025D add rdx,2

; ==================================================================
; §6.2 check_for_door_stairs（全函数主体）
; range 0x1407F33AF-0x1407F37FA
1407F33AF movss xmm12,dword ptr [r8+10h]
1407F33B5 movss xmm6,dword ptr [r8+14h]
1407F33BB movss xmm13,dword ptr [r8+18h]
1407F33C1 mov r14,qword ptr [rcx+8]
1407F33C5 mov r15,qword ptr [rcx+10h]
1407F33C9 add r15,r15
1407F33CC movsd xmm14,mmword ptr [rcx+50h]
1407F33D2 mov qword ptr [rsp+30h],r14
1407F33D7 mov qword ptr [rsp+38h],r15
1407F33DC movsd mmword ptr [rsp+40h],xmm14
1407F33E3 lea rcx,[rsp+30h]
1407F33E8 movaps xmm1,xmm12
1407F33EC movaps xmm2,xmm13
1407F33F0 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407F33F5 movaps xmm8,xmm0
1407F33F9 xorps xmm7,xmm7
1407F33FC maxss xmm8,xmm7
1407F3401 mov rcx,rsi
1407F3404 call _ZN100_$LT$country_core..resources..world_raster_defs..GardenRaster$u20$as$u20$core..ops..deref..Deref$GT$5deref17h37f159664114cf7aE  ; @0x14067DD80
1407F3409 lea rcx,[rax+8]
1407F340D lea rdx,[rax+20h]
1407F3411 lea r8,[rax+10h]
1407F3415 lea r9,[rax+28h]
1407F3419 cmp byte ptr [rax+298h],0
1407F3420 cmovne rcx,rdx
1407F3424 cmove r9,r8
1407F3428 mov rax,qword ptr [r9]
1407F342B mov rcx,qword ptr [rcx]
1407F342E mov qword ptr [rsp+30h],rcx
1407F3433 mov qword ptr [rsp+38h],rax
1407F3438 mov dword ptr [rsp+20h],43020000h
1407F3440 lea rcx,[rsp+30h]
1407F3445 movss xmm3,dword ptr [__real@43020000]
1407F344D movaps xmm1,xmm12
1407F3451 movaps xmm2,xmm13
1407F3455 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h054924d541f34fecE  ; @0x140A14DF0
1407F345A ucomiss xmm0,xmm7
1407F345D movss xmm0,dword ptr [rdi+1Ch]
1407F3462 subss xmm0,dword ptr [rdi+18h]
1407F3467 mulss xmm0,dword ptr [__real@bf000000]
1407F346F addss xmm0,xmm6
1407F3473 jbe 00000001407F349A
1407F3475 movss xmm1,dword ptr [__real@3dcccccd]
1407F347D ucomiss xmm1,xmm0
1407F3480 setae sil
1407F3484 jae 00000001407F34AE
1407F3486 addss xmm0,dword ptr [__real@be4ccccd]
1407F348E ucomiss xmm8,xmm0
1407F3492 jbe 00000001407F37F8
1407F3498 jmp 00000001407F34AE
1407F349A addss xmm0,dword ptr [__real@be4ccccd]
1407F34A2 xor esi,esi
1407F34A4 ucomiss xmm8,xmm0
1407F34A8 jbe 00000001407F37FA
1407F34AE movaps xmm6,xmmword ptr [rbx]
1407F34B1 movaps xmm15,xmm6
1407F34B5 mulps xmm15,xmm6
1407F34B9 movshdup xmm0,xmm15
1407F34BE addss xmm0,xmm15
1407F34C3 movaps xmm1,xmm15
1407F34C7 unpckhpd xmm1,xmm15
1407F34CC addss xmm1,xmm0
1407F34D0 shufps xmm1,xmm1,0
1407F34D4 shufps xmm15,xmm15,0FFh
1407F34D9 subps xmm15,xmm1
1407F34DD movaps xmm1,xmmword ptr [__xmm@000000003f87ae140000000000000000]  ; ->0x142a8af30 f32x4=(0, 0, 1.06, 0)
1407F34E4 mulps xmm1,xmm15
1407F34E8 movaps xmm0,xmmword ptr [__xmm@000000003f87ae140000000000000000]  ; ->0x142a8af30 f32x4=(0, 0, 1.06, 0)
1407F34EF mulps xmm0,xmm6
1407F34F2 movshdup xmm2,xmm0
1407F34F6 addss xmm2,xmm0
1407F34FA movhlps xmm0,xmm0
1407F34FD addss xmm0,xmm2
1407F3501 addss xmm0,xmm0
1407F3505 shufps xmm0,xmm0,0
1407F3509 mulps xmm0,xmm6
1407F350C addps xmm0,xmm1
1407F350F movaps xmm10,xmm6
1407F3513 shufps xmm10,xmm6,0D2h
1407F3518 movaps xmm9,xmmword ptr [__xmm@000000003f87ae140000000000000000]  ; ->0x142a8af30 f32x4=(0, 0, 1.06, 0)
1407F3520 mulps xmm9,xmm10
1407F3524 xorps xmm1,xmm1
1407F3527 mulps xmm1,xmm6
1407F352A movaps xmmword ptr [rsp+70h],xmm1
1407F352F subps xmm9,xmm1
1407F3533 shufps xmm9,xmm9,0D6h
1407F3538 movaps xmm7,xmm6
1407F353B addps xmm7,xmm6
1407F353E shufps xmm7,xmm7,0FFh
1407F3542 mulps xmm9,xmm7
1407F3546 addps xmm9,xmm0
1407F354A movaps xmm11,xmm12
1407F354E addss xmm11,xmm9
1407F3553 movhlps xmm9,xmm9
1407F3557 addss xmm9,xmm13
1407F355C mov qword ptr [rsp+30h],r14
1407F3561 mov qword ptr [rsp+38h],r15
1407F3566 movlps qword ptr [rsp+40h],xmm14
1407F356C lea rcx,[rsp+30h]
1407F3571 movaps xmm1,xmm11
1407F3575 movaps xmm2,xmm9
1407F3579 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407F357E movaps xmm1,xmm8
1407F3582 subss xmm1,xmm0
1407F3586 ucomiss xmm1,dword ptr [__real@3dcccccd]
1407F358D jbe 00000001407F37F8
1407F3593 movss dword ptr [rsp+48h],xmm13
1407F359A movss dword ptr [rsp+4Ch],xmm12
1407F35A1 movaps xmmword ptr [rsp+60h],xmm0
1407F35A6 movss xmm1,dword ptr [rdi+10h]
1407F35AB xorps xmm0,xmm0
1407F35AE mulss xmm0,xmm1
1407F35B2 movaps xmmword ptr [rsp+50h],xmm1
1407F35B7 unpcklps xmm1,xmm0
1407F35BA mulps xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]  ; ->0x1429258e0 f32x4=(0.5, 0.5, 0, 0)
1407F35C1 movaps xmm12,xmm1
1407F35C5 shufps xmm12,xmm1,54h
1407F35CA movaps xmm0,xmm15
1407F35CE mulps xmm0,xmm12
1407F35D2 movaps xmm2,xmm6
1407F35D5 mulps xmm2,xmm12
1407F35D9 movshdup xmm3,xmm2
1407F35DD addss xmm3,xmm2
1407F35E1 movhlps xmm2,xmm2
1407F35E4 addss xmm2,xmm3
1407F35E8 addss xmm2,xmm2
1407F35EC shufps xmm2,xmm2,0
1407F35F0 mulps xmm2,xmm6
1407F35F3 addps xmm2,xmm0
1407F35F6 shufps xmm1,xmm1,0D0h
1407F35FA mulps xmm12,xmm10
1407F35FE mulps xmm1,xmm6
1407F3601 subps xmm12,xmm1
1407F3605 shufps xmm12,xmm12,0D6h
1407F360A mulps xmm12,xmm7
1407F360E addps xmm12,xmm2
1407F3612 movaps xmm13,xmm12
1407F3616 unpckhpd xmm13,xmm12
1407F361B movaps xmm1,xmm11
1407F361F subss xmm1,xmm12
1407F3624 movaps xmm2,xmm9
1407F3628 subss xmm2,xmm13
1407F362D mov qword ptr [rsp+30h],r14
1407F3632 mov qword ptr [rsp+38h],r15
1407F3637 movlps qword ptr [rsp+40h],xmm14
1407F363D lea rcx,[rsp+30h]
1407F3642 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407F3647 movaps xmm1,xmm14
1407F364B movaps xmm14,xmm0
1407F364F addss xmm12,xmm11
1407F3654 addss xmm13,xmm9
1407F3659 mov qword ptr [rsp+30h],r14
1407F365E mov qword ptr [rsp+38h],r15
1407F3663 movlps qword ptr [rsp+40h],xmm1
1407F3668 lea rcx,[rsp+30h]
1407F366D movaps xmm1,xmm12
1407F3671 movaps xmm2,xmm13
1407F3675 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
1407F367A movaps xmm9,xmm14
1407F367E cmpunordss xmm9,xmm14
1407F3684 movaps xmm1,xmm9
1407F3688 andps xmm1,xmm0
1407F368B minss xmm0,xmm14
1407F3690 andnps xmm9,xmm0
1407F3694 orps xmm9,xmm1
1407F3698 movaps xmm2,xmmword ptr [rsp+60h]
1407F369D movaps xmm0,xmm2
1407F36A0 minss xmm0,xmm9
1407F36A5 cmpunordss xmm9,xmm9
1407F36AB movaps xmm1,xmm9
1407F36AF andnps xmm1,xmm0
1407F36B2 andps xmm9,xmm2
1407F36B6 orps xmm9,xmm1
1407F36BA movaps xmm0,xmm8
1407F36BE subss xmm0,xmm9
1407F36C3 movss xmm1,dword ptr [__real@3fc00000]
1407F36CB ucomiss xmm1,xmm0
1407F36CE setbe al
1407F36D1 mov ecx,esi
1407F36D3 not cl
1407F36D5 test cl,al
1407F36D7 jne 00000001407F37F8
1407F36DD movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
1407F36E4 mov ecx,20h
1407F36E9 mov edx,4
1407F36EE call __rust_alloc  ; @0x140613C10
1407F36F3 test rax,rax
1407F36F6 je 00000001407F3863
1407F36FC mov rdi,rax
1407F36FF mov rbx,qword ptr [rsp+170h]
1407F3707 movss xmm1,dword ptr [__real@bf4ccccd]
1407F370F movaps xmm0,xmm1
1407F3712 subss xmm0,xmm9
1407F3717 xorps xmm2,xmm2
1407F371A maxss xmm0,xmm2
1407F371E mulps xmm10,xmmword ptr [__xmm@000000003f8000000000000000000000]  ; ->0x1429cd0c0 f32x4=(0, 0, 1, 0)
1407F3726 subps xmm10,xmmword ptr [rsp+70h]
1407F372C shufps xmm10,xmm10,0D6h
1407F3731 mulps xmm7,xmm10
1407F3735 mulps xmm15,xmmword ptr [__xmm@000000003f8000000000000000000000]  ; ->0x1429cd0c0 f32x4=(0, 0, 1, 0)
1407F373D movaps xmm3,xmmword ptr [__xmm@000000003f8000000000000000000000]  ; ->0x1429cd0c0 f32x4=(0, 0, 1, 0)
1407F3744 mulps xmm3,xmm6
1407F3747 movshdup xmm2,xmm3
1407F374B addss xmm2,xmm3
1407F374F movhlps xmm3,xmm3
1407F3752 addss xmm3,xmm2
1407F3756 addss xmm3,xmm3
1407F375A shufps xmm3,xmm3,0
1407F375E mulps xmm3,xmm6
1407F3761 addps xmm3,xmm15
1407F3765 addps xmm3,xmm7
1407F3768 movaps xmm2,xmm3
1407F376B shufps xmm2,xmm3,0E8h
1407F376F mulps xmm3,xmm3
1407F3772 movaps xmm4,xmm3
1407F3775 unpckhpd xmm4,xmm3
1407F3779 addss xmm4,xmm3
1407F377D sqrtss xmm4,xmm4
1407F3781 movss xmm3,dword ptr [__real@3f800000]
1407F3789 divss xmm3,xmm4
1407F378D test sil,sil
1407F3790 movss xmm4,dword ptr [rsp+4Ch]
1407F3796 movss xmm5,dword ptr [rsp+48h]
1407F379C jne 00000001407F37A2
1407F379E movaps xmm1,xmm9
1407F37A2 movss dword ptr [rdi],xmm4
1407F37A6 movss dword ptr [rdi+4],xmm5
1407F37AB movsldup xmm3,xmm3
1407F37AF mulps xmm2,xmm3
1407F37B2 movlps qword ptr [rdi+8],xmm2
1407F37B6 movss dword ptr [rdi+10h],xmm8
1407F37BC movss dword ptr [rdi+14h],xmm1
1407F37C1 movss dword ptr [rdi+18h],xmm0
1407F37C6 movaps xmm0,xmmword ptr [rsp+50h]
1407F37CB movss dword ptr [rdi+1Ch],xmm0
1407F37D0 mov rcx,qword ptr [rbx+120h]
1407F37D7 test rcx,rcx
1407F37DA je 00000001407F37EC
1407F37DC mov edx,20h
1407F37E1 mov r8d,4
1407F37E7 call __rust_dealloc  ; @0x140613C20
1407F37EC mov qword ptr [rbx+120h],rdi
1407F37F3 mov sil,1
1407F37F6 jmp 00000001407F37FA
1407F37F8 xor esi,esi
1407F37FA mov eax,esi

; ==================================================================
; §6.1 WallAttachedDecoTracker::with_diffs → construct_door_stairs（总外伸 0.56）
; range 0x1412AF878-0x1412AF8F7
1412AF878 movd xmm0,dword ptr [r14]
1412AF87D movd xmm1,dword ptr [r14+4]
1412AF883 movd xmm2,dword ptr [r14+10h]
1412AF889 movss xmm3,dword ptr [r14+14h]
1412AF88F movss xmm4,dword ptr [r14+1Ch]
1412AF895 movss xmm5,dword ptr [r14+18h]
1412AF89B movss xmm7,dword ptr [r14+8]
1412AF8A1 movss xmm8,dword ptr [r14+0Ch]
1412AF8A7 mov rax,qword ptr [rbp+58h]
1412AF8AB mov rax,qword ptr [rax]
1412AF8AE mov qword ptr [rsp+60h],rax
1412AF8B3 mov rax,qword ptr [rbp+50h]
1412AF8B7 mov qword ptr [rsp+58h],rax
1412AF8BC lea rax,[rbp+80h]
1412AF8C3 mov qword ptr [rsp+50h],rax
1412AF8C8 movss dword ptr [rsp+48h],xmm8
1412AF8CF movss dword ptr [rsp+40h],xmm7
1412AF8D5 lea rax,[rbp+40h]
1412AF8D9 mov qword ptr [rsp+38h],rax
1412AF8DE movss dword ptr [rsp+28h],xmm5
1412AF8E4 movss dword ptr [rsp+20h],xmm4
1412AF8EA mov dword ptr [rsp+30h],3F0F5C29h
1412AF8F2 call _ZN16system_decorator16decorator_visual27door_stairs_assemble_bricks21construct_door_stairs17h0fba84f8598d218aE  ; @0x14200C7F0
1412AF8F7 jmp 00000001412AF7C0  ; @0x1412AF7C0

; ==================================================================
; §6.3 construct_door_stairs：层数/级数 clamp(ceil(H/0.2),2,5)/横向 max(ceil(w/0.45),3)
; range 0x14200C864-0x14200CC27
14200C864 movaps xmm8,xmm3
14200C868 movaps xmm9,xmm2
14200C86C movss dword ptr [rbp+178h],xmm1
14200C874 movss dword ptr [rbp+17Ch],xmm0
14200C87C mov rsi,qword ptr [rbp+308h]
14200C883 movss xmm6,dword ptr [rbp+2F8h]
14200C88B movaps xmm10,xmm2
14200C88F subss xmm10,xmm3
14200C894 movaps xmm7,xmm10
14200C898 addss xmm7,xmm6
14200C89C movaps xmm0,xmm7
14200C89F divss xmm0,dword ptr [__real@3e4ccccd]
14200C8A7 call ceilf  ; @0x142923E60
14200C8AC cvttss2si rax,xmm0
14200C8B1 mov rcx,rax
14200C8B4 sar rcx,3Fh
14200C8B8 movaps xmm1,xmm0
14200C8BB subss xmm1,dword ptr [__real@5f000000]
14200C8C3 cvttss2si rdx,xmm1
14200C8C8 and rdx,rcx
14200C8CB or rdx,rax
14200C8CE xor eax,eax
14200C8D0 xorps xmm1,xmm1
14200C8D3 ucomiss xmm0,xmm1
14200C8D6 cmovae rax,rdx
14200C8DA ucomiss xmm0,dword ptr [__real@5f7fffff]
14200C8E1 mov rcx,0FFFFFFFFFFFFFFFFh
14200C8E8 cmovbe rcx,rax
14200C8EC cmp rcx,3
14200C8F0 mov edx,2
14200C8F5 cmovae rdx,rcx
14200C8F9 movss xmm2,dword ptr [__real@3d23d70b]
14200C901 divss xmm2,xmm7
14200C905 lea rax,[142FCE968h]  ; ->0x142fce968 &str"crates/systems/decorator/src/decorator_visual/door_stairs_assemble_bricks.rs"
14200C90C mov qword ptr [rsp+20h],rax
14200C911 lea rcx,[rbp+100h]
14200C918 mov r9,rsi
14200C91B call _ZN5utils13random_splits17h445a73628407a507E  ; @0x140C90350
14200C920 mov r15,qword ptr [rbp+108h]
14200C927 mov r14,qword ptr [rbp+110h]
14200C92E lea rsi,[r14*4]
14200C936 test r14,r14
14200C939 je 000000014200C97C
14200C93B movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
14200C942 mov edx,4
14200C947 mov rcx,rsi
14200C94A call __rust_alloc  ; @0x140613C10
14200C94F test rax,rax
14200C952 je 000000014200D65F
14200C958 mov rbx,rax
14200C95B subss xmm8,xmm6
14200C960 cmp r14,8
14200C964 setae al
14200C967 mov rcx,rbx
14200C96A sub rcx,r15
14200C96D cmp rcx,20h
14200C971 setae cl
14200C974 test al,cl
14200C976 jne 000000014200C986
14200C978 xor eax,eax
14200C97A jmp 000000014200C9F4
14200C97C mov ebx,4
14200C981 jmp 000000014200CA8B  ; @0x14200CA8B
14200C986 mov rax,r14
14200C989 and rax,0FFFFFFFFFFFFFFF8h
14200C98D movaps xmm0,xmm9
14200C991 shufps xmm0,xmm9,0
14200C996 movaps xmm1,xmm8
14200C99A shufps xmm1,xmm8,0
14200C99F xor ecx,ecx
14200C9A1 movaps xmm2,xmmword ptr [__xmm@3f8000003f8000003f8000003f800000]  ; ->0x142925930 f32x4=(1, 1, 1, 1)
14200C9B0 movups xmm3,xmmword ptr [r15+rcx*4]
14200C9B5 movups xmm4,xmmword ptr [r15+rcx*4+10h]
14200C9BB movaps xmm5,xmm2
14200C9BE subps xmm5,xmm3
14200C9C1 movaps xmm6,xmm2
14200C9C4 subps xmm6,xmm4
14200C9C7 mulps xmm5,xmm0
14200C9CA mulps xmm6,xmm0
14200C9CD mulps xmm3,xmm1
14200C9D0 addps xmm3,xmm5
14200C9D3 mulps xmm4,xmm1
14200C9D6 addps xmm4,xmm6
14200C9D9 movups xmmword ptr [rbx+rcx*4],xmm3
14200C9DD movups xmmword ptr [rbx+rcx*4+10h],xmm4
14200C9E2 add rcx,8
14200C9E6 cmp rax,rcx
14200C9E9 jne 000000014200C9B0
14200C9EB cmp r14,rax
14200C9EE je 000000014200CA8B
14200C9F4 mov rcx,rax
14200C9F7 or rcx,1
14200C9FB test r14b,1
14200C9FF je 000000014200CA29
14200CA01 movss xmm0,dword ptr [r15+rax*4]
14200CA07 movss xmm1,dword ptr [__real@3f800000]
14200CA0F subss xmm1,xmm0
14200CA13 mulss xmm1,xmm9
14200CA18 mulss xmm0,xmm8
14200CA1D addss xmm0,xmm1
14200CA21 movss dword ptr [rbx+rax*4],xmm0
14200CA26 mov rax,rcx
14200CA29 cmp r14,rcx
14200CA2C je 000000014200CA8B
14200CA2E movss xmm0,dword ptr [__real@3f800000]
14200CA40 movss xmm1,dword ptr [r15+rax*4]
14200CA46 movaps xmm2,xmm0
14200CA49 subss xmm2,xmm1
14200CA4D mulss xmm2,xmm9
14200CA52 mulss xmm1,xmm8
14200CA57 addss xmm1,xmm2
14200CA5B movss dword ptr [rbx+rax*4],xmm1
14200CA60 movss xmm1,dword ptr [r15+rax*4+4]
14200CA67 movaps xmm2,xmm0
14200CA6A subss xmm2,xmm1
14200CA6E mulss xmm2,xmm9
14200CA73 mulss xmm1,xmm8
14200CA78 addss xmm1,xmm2
14200CA7C movss dword ptr [rbx+rax*4+4],xmm1
14200CA82 add rax,2
14200CA86 cmp r14,rax
14200CA89 jne 000000014200CA40
14200CA8B mov qword ptr [rbp+0F0h],rsi
14200CA92 mov rdi,qword ptr [rbp+320h]
14200CA99 movss xmm11,dword ptr [rbp+300h]
14200CAA2 movss xmm13,dword ptr [rbp+2F0h]
14200CAAB mov rdx,qword ptr [rbp+100h]
14200CAB2 test rdx,rdx
14200CAB5 je 000000014200CAC9
14200CAB7 shl rdx,2
14200CABB mov r8d,4
14200CAC1 mov rcx,r15
14200CAC4 call __rust_dealloc  ; @0x140613C20
14200CAC9 movss xmm1,dword ptr [rbp+318h]
14200CAD1 movss xmm9,dword ptr [rbp+310h]
14200CADA divss xmm10,dword ptr [__real@3e4ccccd]
14200CAE3 movaps xmm0,xmm10
14200CAE7 movaps xmm10,xmm1
14200CAEB call ceilf  ; @0x142923E60
14200CAF0 movss xmm6,dword ptr [__real@5f000000]
14200CAF8 movaps xmm1,xmm0
14200CAFB subss xmm1,xmm6
14200CAFF cvttss2si rax,xmm1
14200CB04 cvttss2si rcx,xmm0
14200CB09 mov rdx,rcx
14200CB0C sar rdx,3Fh
14200CB10 and rdx,rax
14200CB13 or rdx,rcx
14200CB16 xor r15d,r15d
14200CB19 xorps xmm7,xmm7
14200CB1C ucomiss xmm0,xmm7
14200CB1F cmovb rdx,r15
14200CB23 movss xmm8,dword ptr [__real@5f7fffff]
14200CB2C ucomiss xmm0,xmm8
14200CB30 mov rsi,0FFFFFFFFFFFFFFFFh
14200CB37 cmova rdx,rsi
14200CB3B cmp rdx,5
14200CB3F mov eax,5
14200CB44 cmovb rax,rdx
14200CB48 cmp rax,3
14200CB4C mov ecx,2
14200CB51 cmovae rcx,rax
14200CB55 mov qword ptr [rbp+0C0h],rcx
14200CB5C xorps xmm0,xmm0
14200CB5F cvtsi2ss xmm0,rcx
14200CB64 divss xmm11,xmm0
14200CB69 movaps xmm0,xmm13
14200CB6D divss xmm0,dword ptr [__real@3ee66666]
14200CB75 call ceilf  ; @0x142923E60
14200CB7A movaps xmm1,xmm0
14200CB7D subss xmm1,xmm6
14200CB81 cvttss2si rax,xmm1
14200CB86 cvttss2si rcx,xmm0
14200CB8B mov rdx,rcx
14200CB8E sar rdx,3Fh
14200CB92 and rdx,rax
14200CB95 or rdx,rcx
14200CB98 ucomiss xmm0,xmm7
14200CB9B mov r8d,0
14200CBA1 cmovb rdx,r15
14200CBA5 ucomiss xmm0,xmm8
14200CBA9 cmova rdx,rsi
14200CBAD cmp rdx,4
14200CBB1 mov eax,3
14200CBB6 cmovae rax,rdx
14200CBBA mov qword ptr [rbp+0A8h],rax
14200CBC1 mov qword ptr [rbp+0E8h],r14
14200CBC8 lea rax,[rbx+r14*4]
14200CBCC mov qword ptr [rbp+0E0h],rax
14200CBD3 movss xmm0,dword ptr [__real@3dcccccd]
14200CBDB addss xmm0,xmm11
14200CBE0 movss dword ptr [rbp+1C4h],xmm0
14200CBE8 lea rsi,[rbp-20h]
14200CBEC lea r13,[rbp+80h]
14200CBF3 movss xmm6,dword ptr [__real@3e3851eb]
14200CBFB divss xmm6,xmm13
14200CC00 mulss xmm13,dword ptr [__real@3f000000]
14200CC09 mov rax,qword ptr [rdi]
14200CC0C mov qword ptr [rbp+0D0h],rax
14200CC13 mov rax,qword ptr [rdi+8]
14200CC17 mov qword ptr [rbp+0D8h],rax
14200CC1E movaps xmm15,xmm13
14200CC22 shufps xmm15,xmm13,0
14200CC27 mov al,1

; ==================================================================
; §6.3 construct_door_stairs：第 j 排距门 j*run + 0.28
; range 0x14200CF4D-0x14200D005
14200CF4D mov rax,qword ptr [rbp+180h]
14200CF54 xorps xmm0,xmm0
14200CF57 cvtsi2ss xmm0,rax
14200CF5C inc rax
14200CF5F mov qword ptr [rbp+180h],rax
14200CF66 mulss xmm0,xmm11
14200CF6B addss xmm0,xmm7
14200CF6F test rbx,rbx
14200CF72 je 000000014200CD95
14200CF78 cmp rbx,1
14200CF7C je 000000014200CD80
14200CF82 mov qword ptr [rbp+188h],rbx
14200CF89 movaps xmm3,xmm0
14200CF8C movaps xmm4,xmm0
14200CF8F mulss xmm4,xmm10
14200CF94 addss xmm4,dword ptr [rbp+178h]
14200CF9C mulss xmm3,xmm9
14200CFA1 addss xmm3,dword ptr [rbp+17Ch]
14200CFA9 movss xmm6,dword ptr [r15]
14200CFAE mov qword ptr [rbp+190h],r15
14200CFB5 movss xmm7,dword ptr [r15+4]
14200CFBB movaps xmm0,xmm6
14200CFBE movss xmm2,dword ptr [__real@3f000000]
14200CFC6 mulss xmm0,xmm2
14200CFCA movaps xmm1,xmm7
14200CFCD mulss xmm1,xmm2
14200CFD1 addss xmm1,xmm0
14200CFD5 movaps xmm0,xmm1
14200CFD8 mulss xmm0,xmm9
14200CFDD mulss xmm1,xmm10
14200CFE2 movss dword ptr [rbp+170h],xmm3
14200CFEA movaps xmm2,xmm3
14200CFED subss xmm2,xmm1
14200CFF1 movss dword ptr [rbp+174h],xmm4
14200CFF9 addss xmm0,xmm4
14200CFFD movss dword ptr [rbp+100h],xmm2
14200D005 movss dword ptr [rbp+104h],xmm0

; ==================================================================
; §3.2 split_segment_into_supported_and_unsupported_subsegments：-1.17 m 起、朝墙 1.78 m 射线
; range 0x1407D9811-0x1407D9A00
1407D9811 mov rcx,qword ptr [rbp+190h]
1407D9818 call _ZN12country_core9resources6stairs22stairs_assembly_params15StairWallAnchor11wall_normal17h1e20e237088a288eE  ; @0x140959E90
1407D981D cmp r14,qword ptr [rbp-8]
1407D9821 jae 00000001407D9B94
1407D9827 movaps xmm7,xmm0
1407D982A movaps xmm8,xmm1
1407D982E movss xmm14,dword ptr [rsi+r14*8]
1407D9834 movss xmm15,dword ptr [rsi+r14*8+4]
1407D983B movaps xmm0,xmm15
1407D983F xorps xmm0,xmm9
1407D9843 movss dword ptr [rbp-20h],xmm0
1407D9848 movss dword ptr [rbp-1Ch],xmm14
1407D984E lea rcx,[rbp-58h]
1407D9852 lea rdx,[rbp-20h]
1407D9856 call _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E  ; @0x140C9CDC0
1407D985B mulss xmm7,xmm15
1407D9860 mulss xmm8,xmm14
1407D9865 xor eax,eax
1407D9867 ucomiss xmm8,xmm7
1407D986B seta al
1407D986E lea rcx,[__real@bf8000003f800000]
1407D9875 movss xmm1,dword ptr [rcx+rax*4]
1407D987A movss xmm0,dword ptr [rbp-50h]
1407D987F mulss xmm0,xmm1
1407D9883 movaps xmm2,xmm0
1407D9886 mulss xmm2,xmm0
1407D988A movsd xmm3,mmword ptr [rbp-58h]
1407D988F movsldup xmm1,xmm1
1407D9893 mulps xmm1,xmm3
1407D9896 movaps xmm3,xmm1
1407D9899 mulps xmm3,xmm1
1407D989C movshdup xmm4,xmm3
1407D98A0 addss xmm4,xmm3
1407D98A4 addss xmm4,xmm2
1407D98A8 xorps xmm3,xmm3
1407D98AB sqrtss xmm3,xmm4
1407D98AF movaps xmm2,xmm10
1407D98B3 divss xmm2,xmm3
1407D98B7 movd eax,xmm2
1407D98BB test eax,eax
1407D98BD setns cl
1407D98C0 mov edx,eax
1407D98C2 and edx,7FFFFFFFh
1407D98C8 add edx,0FF800000h
1407D98CE cmp edx,7F000000h
1407D98D4 setb dl
1407D98D7 and dl,cl
1407D98D9 dec eax
1407D98DB cmp eax,7FFFFFh
1407D98E0 setb al
1407D98E3 or al,dl
1407D98E5 jne 00000001407D98F0
1407D98E7 xorps xmm0,xmm0
1407D98EA jmp 00000001407D98F4
1407D98F0 mulss xmm0,xmm2
1407D98F4 cmp r14,qword ptr [rbp]
1407D98F8 jae 00000001407D9B7C
1407D98FE cmp r14,qword ptr [rbp+60h]
1407D9902 jae 00000001407D9B64
1407D9908 movsldup xmm2,xmm2
1407D990C mulps xmm1,xmm2
1407D990F movzx eax,al
1407D9912 movd xmm2,eax
1407D9916 pshufd xmm2,xmm2,50h
1407D991B pslld xmm2,1Fh
1407D9920 psrad xmm2,1Fh
1407D9925 pand xmm2,xmm1
1407D9929 lea rax,[r14+r14*2]
1407D992D movss xmm1,dword ptr [r12+rax*4+4]
1407D9934 addss xmm1,xmm11
1407D9939 movss xmm3,dword ptr [rdi+r14*4]
1407D993F movaps xmm7,xmm0
1407D9942 mulss xmm7,xmm3
1407D9946 mulss xmm7,xmm12
1407D994B addss xmm7,dword ptr [r12+rax*4+8]
1407D9952 movsldup xmm3,xmm3
1407D9956 mulps xmm3,xmm2
1407D9959 mulps xmm3,xmm13
1407D995D movss xmm8,dword ptr [r12+rax*4]
1407D9963 unpcklps xmm8,xmm1
1407D9967 addps xmm8,xmm3
1407D996B movlps qword ptr [rbp-20h],xmm8
1407D9970 movss dword ptr [rbp-18h],xmm7
1407D9975 movlpd qword ptr [rbp-14h],xmm2
1407D997A movss dword ptr [rbp-0Ch],xmm0
1407D997F mov dword ptr [rsp+20h],1
1407D9987 lea rcx,[rbp-58h]
1407D998B mov rdx,qword ptr [rbp-30h]
1407D998F lea r8,[rbp-20h]
1407D9993 movaps xmm3,xmm6
1407D9996 call _ZN12country_core7systems9collision5world12RaycastWorld14raycast_simple17hf5f9373685c2274fE  ; @0x1408CCC20
1407D999B cmp byte ptr [rbp-58h],11h
1407D999F jne 00000001407D99D0
1407D99A1 mov eax,1
1407D99A6 test r13,r13
1407D99A9 je 00000001407D9B09
1407D99AF mov r13,rax
1407D99B2 inc r14
1407D99B5 cmp r14,qword ptr [rbp+68h]
1407D99B9 jb 00000001407D9811
1407D99BF jmp 00000001407D965B  ; @0x1407D965B
1407D99D0 subss xmm8,dword ptr [rbp-40h]
1407D99D6 subss xmm7,dword ptr [rbp-38h]
1407D99DB mulss xmm8,xmm8
1407D99E0 mulss xmm7,xmm7
1407D99E4 addss xmm7,xmm8
1407D99E9 xorps xmm0,xmm0
1407D99EC sqrtss xmm0,xmm7
1407D99F0 movss xmm7,dword ptr [rdi+r14*4]
1407D99F6 mulss xmm7,xmm12
1407D99FB addss xmm7,xmm0
1407D99FF test r13b,1

; ==================================================================
; §4.2 playermade_stairs_assemble：墙重叠 → 洞半宽 0.76/0.66、洞顶 10 m
; range 0x14001D8E8-0x14001DB44
14001D8E8 test byte ptr [r12],1
14001D8ED je 000000014001D979
14001D8F3 mov rcx,r15
14001D8F6 call _ZN12country_core9resources5walls17public_wall_state15PublicWallState11flat_roof_y17he6f49c9b65aa2c97E  ; @0x140B2A6E0
14001D8FB lea rdx,[r12+4]
14001D900 addss xmm0,dword ptr [__real@be99999a]
14001D908 lea rcx,[rbp+6C0h]
14001D90F movaps xmm2,xmm0
14001D912 call _ZN5utils8geometry9rectangle11Rectangle2d10as_points317hef2ca4ed70315ebbE  ; @0x140C98B90
14001D917 movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
14001D91E mov ecx,30h
14001D923 mov edx,4
14001D928 call __rust_alloc  ; @0x140613C10
14001D92D test rax,rax
14001D930 je 00000001400209D3
14001D936 movups xmm0,xmmword ptr [rbp+6C0h]
14001D93D movups xmm1,xmmword ptr [rbp+6D0h]
14001D944 movups xmm2,xmmword ptr [rbp+6E0h]
14001D94B movups xmmword ptr [rax+20h],xmm2
14001D94F movups xmmword ptr [rax+10h],xmm1
14001D953 mov qword ptr [rbp+7A0h],rax
14001D95A movups xmmword ptr [rax],xmm0
14001D95D lea rbx,[r12+0Ch]
14001D962 add r12,10h
14001D966 mov eax,4
14001D96B mov qword ptr [rbp+808h],rax
14001D972 mov ecx,4
14001D977 jmp 000000014001D9DD
14001D979 mov rcx,r15
14001D97C call _ZN12country_core9resources5walls17public_wall_state15PublicWallState11flat_roof_y17he6f49c9b65aa2c97E  ; @0x140B2A6E0
14001D981 lea rbx,[r12+4]
14001D986 addss xmm0,dword ptr [__real@be99999a]
14001D98E lea rcx,[rbp+6C0h]
14001D995 mov rdx,rbx
14001D998 movss xmm2,dword ptr [__real@3ecccccd]
14001D9A0 movaps xmm3,xmm0
14001D9A3 call _ZN5utils8geometry6circle8Circle2d10as_points317h34d390af55da2cfeE  ; @0x140C97C50
14001D9A8 lea rcx,[rbp+50h]
14001D9AC lea rdx,[rbp+6C0h]
14001D9B3 lea r8,[anon.5315650f31121684c12080fdd7b7c2a1.21.llvm.124838408796408342]  ; ->0x1429de088 &str"/rustc/05f9846f893b09a1be1fc8560e33fc3c815cfecb\library\core\src\iter\traits\iterator.rs"
14001D9BA call _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17h8418c9545461ed27E  ; @0x1402BD6B0
14001D9BF add r12,8
14001D9C3 mov rax,qword ptr [rbp+50h]
14001D9C7 mov qword ptr [rbp+808h],rax
14001D9CE mov rax,qword ptr [rbp+58h]
14001D9D2 mov qword ptr [rbp+7A0h],rax
14001D9D9 mov rcx,qword ptr [rbp+60h]
14001D9DD mov dword ptr [rbp+6B0h],esi
14001D9E3 movss xmm0,dword ptr [r12]
14001D9E9 movss xmm1,dword ptr [rbx]
14001D9ED movss dword ptr [rbp+680h],xmm1
14001D9F5 movss dword ptr [rbp+684h],xmm0
14001D9FD lea rax,[rcx*4]
14001DA05 lea rbx,[rax+rax*2]
14001DA09 mov qword ptr [rbp+7F0h],rcx
14001DA10 test rcx,rcx
14001DA13 je 000000014001DA37
14001DA15 movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
14001DA1C mov edx,4
14001DA21 mov rcx,rbx
14001DA24 call __rust_alloc  ; @0x140613C10
14001DA29 test rax,rax
14001DA2C je 00000001400209A1
14001DA32 mov rsi,rax
14001DA35 jmp 000000014001DA3C
14001DA37 mov esi,4
14001DA3C mov r12,qword ptr [rbp+7A0h]
14001DA43 lea rax,[r12+rbx]
14001DA47 mov qword ptr [rbp+7D0h],rax
14001DA4E mov rcx,rsi
14001DA51 mov rdx,r12
14001DA54 mov r8,rbx
14001DA57 call memcpy  ; @0x142923C00
14001DA5C add rbx,rsi
14001DA5F mov rax,qword ptr [rbp+7F0h]
14001DA66 mov qword ptr [rbp+6C0h],rax
14001DA6D mov qword ptr [rbp+6C8h],rsi
14001DA74 mov qword ptr [rbp+6D0h],rsi
14001DA7B mov qword ptr [rbp+6D8h],rax
14001DA82 mov qword ptr [rbp+6E0h],rbx
14001DA89 mov qword ptr [rbp+6E8h],r12
14001DA90 mov qword ptr [rbp+6F0h],r12
14001DA97 mov rax,qword ptr [rbp+808h]
14001DA9E mov qword ptr [rbp+6F8h],rax
14001DAA5 mov rax,qword ptr [rbp+7D0h]
14001DAAC mov qword ptr [rbp+700h],rax
14001DAB3 mov dword ptr [rbp+708h],0
14001DABD movss xmm0,dword ptr [__real@3f428f5d]
14001DAC5 movss dword ptr [rbp+558h],xmm0
14001DACD test byte ptr [rbp+6B0h],1
14001DAD4 jne 000000014001DAE6
14001DAD6 movss xmm0,dword ptr [__real@3f28f5c3]
14001DADE movss dword ptr [rbp+558h],xmm0
14001DAE6 xorps xmm0,xmm0
14001DAE9 jmp 000000014001DB44
14001DAF0 movss xmm0,dword ptr [rbp+7D8h]
14001DAF8 subss xmm0,xmm7
14001DAFC mov rax,qword ptr [r12+8]
14001DB01 lea rcx,[rsi+rsi*2]
14001DB05 mov rdx,qword ptr [rbp+790h]
14001DB0C mov qword ptr [rax+rcx*8],rdx
14001DB10 movss dword ptr [rax+rcx*8+8],xmm12
14001DB17 movss dword ptr [rax+rcx*8+0Ch],xmm11
14001DB1E movss dword ptr [rax+rcx*8+10h],xmm0
14001DB24 mov dword ptr [rax+rcx*8+14h],41200000h
14001DB2C inc rsi
14001DB2F mov qword ptr [r12+10h],rsi
14001DB34 movss xmm0,dword ptr [rbp+7F0h]
14001DB3C addss xmm0,dword ptr [rbp+7D0h]
14001DB44 movss dword ptr [rbp+7F0h],xmm0

; ==================================================================
; §4.2 playermade_stairs_add_wall_holes：WallHoles::add(wall, aabb, origin=4)
; range 0x1407D1FA0-0x1407D209B
1407D1FA0 mov r14d,dword ptr [rdx+1Ch]
1407D1FA4 mov r13,qword ptr [rdx]
1407D1FA7 mov rsi,qword ptr [rdx+10h]
1407D1FAB mov r15,qword ptr [rax]
1407D1FAE lea r12,[r15+10h]
1407D1FB2 movdqa xmm0,xmmword ptr [r15]
1407D1FB7 pmovmskb r11d,xmm0
1407D1FBC not r11d
1407D1FBF lea rbp,[rsp+30h]
1407D1FC4 jmp 00000001407D1FE3
1407D1FD0 mov r10,qword ptr [rsp+28h]
1407D1FD5 dec r10
1407D1FD8 mov r11,qword ptr [rsp+20h]
1407D1FDD je 00000001407D20A0
1407D1FE3 test r11w,r11w
1407D1FE7 mov qword ptr [rsp+28h],r10
1407D1FEC je 00000001407D2000
1407D1FEE lea eax,[r11-1]
1407D1FF2 and eax,r11d
1407D1FF5 jmp 00000001407D202F
1407D2000 movdqa xmm0,xmmword ptr [r12]
1407D2006 pmovmskb r11d,xmm0
1407D200B add r15,0FFFFFFFFFFFFEF00h
1407D2012 add r12,10h
1407D2016 cmp r11d,0FFFFh
1407D201D je 00000001407D2000
1407D201F mov edx,0FFFFFFFEh
1407D2024 sub edx,r11d
1407D2027 not r11d
1407D202A mov eax,r11d
1407D202D and eax,edx
1407D202F mov edx,r11d
1407D2032 tzcnt edx,edx
1407D2036 mov qword ptr [rsp+20h],rax
1407D203B neg rdx
1407D203E imul rax,rdx,110h
1407D2045 mov rdx,qword ptr [r15+rax-0C8h]
1407D204D test rdx,rdx
1407D2050 je 00000001407D1FD0
1407D2056 mov rax,qword ptr [r15+rax-0D0h]
1407D205E lea rdx,[rdx+rdx*2]
1407D2062 lea rbx,[rax+rdx*8]
1407D2070 lea rdi,[rax+18h]
1407D2074 mov rdx,qword ptr [rax]
1407D2077 movdqu xmm0,xmmword ptr [rax+8]
1407D207C movdqa xmmword ptr [rsp+30h],xmm0
1407D2082 mov dword ptr [rsi],r14d
1407D2085 mov rcx,r13
1407D2088 mov r8,rbp
1407D208B mov r9b,4
1407D208E call _ZN12country_core9resources5walls10wall_holes9WallHoles3add17h405bcfecc6cfda40E  ; @0x140F86500
1407D2093 mov rax,rdi
1407D2096 cmp rdi,rbx
1407D2099 jne 00000001407D2070
1407D209B jmp 00000001407D1FD0  ; @0x1407D1FD0

; ==================================================================
; §3.3 arches_n_walls：拱脚 raycast + StairsPillarProposals::add
; range 0x140498B66-0x140498DCE
140498B66 movsd xmm0,mmword ptr [__xmm@0000000000000000bf80000080000000]  ; ->0x1429ddd00 f64=-0.0078125 u64=0xbf80000080000000
140498B6E movsd mmword ptr [rbp+64Ch],xmm0
140498B76 mov dword ptr [rbp+654h],80000000h
140498B80 mov rax,qword ptr [rbp+800h]
140498B87 mov rdx,qword ptr [rax]
140498B8A mov dword ptr [rsp+20h],202h
140498B92 lea rcx,[rbp+560h]
140498B99 lea r8,[rbp+640h]
140498BA0 movss xmm3,dword ptr [__real@7f7fffff]
140498BA8 call _ZN12country_core7systems9collision5world12RaycastWorld14raycast_simple17hf5f9373685c2274fE  ; @0x1408CCC20
140498BAD movzx eax,byte ptr [rbp+560h]
140498BB4 cmp eax,11h
140498BB7 jne 0000000140498C1C
140498BB9 mov rdx,qword ptr [rbp+3A0h]
140498BC0 cmp rsi,rdx
140498BC3 jae 000000014049A4B4
140498BC9 mov rax,qword ptr [rbp+398h]
140498BD0 mov rcx,qword ptr [rbp+548h]
140498BD7 mov rcx,qword ptr [rcx]
140498BDA movss xmm1,dword ptr [rax+rsi*8]
140498BDF movss xmm2,dword ptr [rax+rsi*8+4]
140498BE5 mov rax,qword ptr [rcx+8]
140498BE9 mov rdx,qword ptr [rcx+10h]
140498BED add rdx,rdx
140498BF0 mov qword ptr [rbp+480h],rax
140498BF7 mov qword ptr [rbp+488h],rdx
140498BFE movsd xmm0,mmword ptr [rcx+50h]
140498C03 movsd mmword ptr [rbp+490h],xmm0
140498C0B lea rcx,[rbp+480h]
140498C12 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
140498C17 jmp 0000000140498CB0  ; @0x140498CB0
140498C1C movss xmm0,dword ptr [rbp+57Ch]
140498C24 cmp eax,1
140498C27 je 0000000140498C47
140498C29 cmp eax,2
140498C2C jne 0000000140498C4E
140498C2E movaps xmm1,xmm0
140498C31 addss xmm1,dword ptr [__real@bf800000]
140498C39 test byte ptr [rbp+561h],1
140498C40 jne 0000000140498CB0
140498C42 movaps xmm0,xmm1
140498C45 jmp 0000000140498CB0
140498C47 addss xmm0,xmm14
140498C4C jmp 0000000140498CB0
140498C4E mov rdx,qword ptr [rbp+3A0h]
140498C55 cmp rsi,rdx
140498C58 jae 000000014049A64C
140498C5E mov rax,qword ptr [rbp+398h]
140498C65 mov rcx,qword ptr [rbp+548h]
140498C6C mov rcx,qword ptr [rcx]
140498C6F movss xmm1,dword ptr [rax+rsi*8]
140498C74 movss xmm2,dword ptr [rax+rsi*8+4]
140498C7A mov rax,qword ptr [rcx+8]
140498C7E mov rdx,qword ptr [rcx+10h]
140498C82 add rdx,rdx
140498C85 mov qword ptr [rbp+480h],rax
140498C8C mov qword ptr [rbp+488h],rdx
140498C93 movsd xmm0,mmword ptr [rcx+50h]
140498C98 movsd mmword ptr [rbp+490h],xmm0
140498CA0 lea rcx,[rbp+480h]
140498CA7 call _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915  ; @0x140257BA0
140498CB0 mov eax,dword ptr [rbp+3D4h]
140498CB6 mov rcx,qword ptr [rbp+180h]
140498CBD mov dword ptr [rcx],eax
140498CBF test r15b,r15b
140498CC2 je 0000000140498CE2
140498CC4 mov rdx,qword ptr [rbp+610h]
140498CCB cmp rsi,rdx
140498CCE jae 000000014049A4E3
140498CD4 mov rax,qword ptr [rbp+608h]
140498CDB movss xmm1,dword ptr [rax+rsi*4]
140498CE0 jmp 0000000140498D33
140498CE2 cmp rsi,qword ptr [rbp+638h]
140498CE9 jae 000000014049A4C8
140498CEF movss xmm2,dword ptr [rbp+350h]
140498CF7 mov rax,qword ptr [rbp+6C8h]
140498CFE movss xmm3,dword ptr [rax+rsi*4]
140498D03 movaps xmm1,xmm2
140498D06 cmpunordss xmm1,xmm2
140498D0B movaps xmm4,xmm1
140498D0E andps xmm4,xmm3
140498D11 maxss xmm3,xmm2
140498D15 andnps xmm1,xmm3
140498D18 orps xmm1,xmm4
140498D1B movaps xmm2,xmm0
140498D1E maxss xmm2,xmm1
140498D22 cmpunordss xmm1,xmm1
140498D27 movaps xmm3,xmm1
140498D2A andnps xmm3,xmm2
140498D2D andps xmm1,xmm0
140498D30 orps xmm1,xmm3
140498D33 mov rdx,qword ptr [rbp+680h]
140498D3A cmp rsi,rdx
140498D3D jae 000000014049A40C
140498D43 mov rdx,qword ptr [rbp+540h]
140498D4A cmp rsi,rdx
140498D4D jae 000000014049A420
140498D53 mov rax,qword ptr [rbp+538h]
140498D5A mov rcx,qword ptr [rbp+678h]
140498D61 movss xmm2,dword ptr [rax+rsi*4]
140498D66 movlps qword ptr [rbp+560h],xmm8
140498D6E movss dword ptr [rbp+568h],xmm1
140498D76 movss dword ptr [rbp+56Ch],xmm0
140498D7E movsd xmm0,mmword ptr [rcx+rsi*8]
140498D83 movsd mmword ptr [rbp+570h],xmm0
140498D8B movss dword ptr [rbp+578h],xmm2
140498D93 mov eax,dword ptr [rbp+5FCh]
140498D99 mov dword ptr [rbp+57Ch],eax
140498D9F mov rax,qword ptr [rbp+6C0h]
140498DA6 mov qword ptr [rbp+580h],rax
140498DAD mov byte ptr [rbp+588h],r13b
140498DB4 mov rcx,qword ptr [rbp+188h]
140498DBB lea rdx,[rbp+158h]
140498DC2 lea r8,[rbp+560h]
140498DC9 call _ZN12country_core9resources12stairs_walls21StairsPillarProposals3add17h4b3a3d4dae3cb010E  ; @0x140901270
140498DCE mov rcx,rbx

; ==================================================================
; §3.3 preprocess_curve：拱数 max(round((L - 1.6/3.2)/4), 1)、端部内缩 min(0.368/L, 0.2)
; range 0x140780F1F-0x140781147
140780F1F movss xmm7,dword ptr [rbp+30h]
140780F24 movss xmm6,dword ptr [__real@3ebc6a7f]
140780F2C divss xmm6,xmm7
140780F30 minss xmm6,dword ptr [__real@3e4ccccd]
140780F38 cmp byte ptr [rdi],0
140780F3B movaps xmm0,xmm6
140780F3E jne 0000000140780F43
140780F40 xorps xmm0,xmm0
140780F43 movss dword ptr [rbp+15Ch],xmm0
140780F4B movss xmm0,dword ptr [__real@3f800000]
140780F53 cmp byte ptr [rdi+1],0
140780F57 je 0000000140780F5D
140780F59 subss xmm0,xmm6
140780F5D movss dword ptr [rbp+1BCh],xmm0
140780F65 mov qword ptr [rbp+1C0h],0
140780F70 mov qword ptr [rbp+1C8h],4
140780F7B mov qword ptr [rbp+1D0h],0
140780F86 movzx eax,byte ptr [__rust_no_alloc_shim_is_unstable]  ; ->0x143634c09 (.data bss)
140780F8D mov ecx,10h
140780F92 mov edx,8
140780F97 call __rust_alloc  ; @0x140613C10
140780F9C test rax,rax
140780F9F je 00000001407822FF
140780FA5 movzx ebx,word ptr [rbp+320h]
140780FAC mov qword ptr [rax],0
140780FB3 mov byte ptr [rax+8],0
140780FB7 mov qword ptr [rbp+1F0h],1
140780FC2 mov qword ptr [rbp+1F8h],rax
140780FC9 mov qword ptr [rbp+200h],1
140780FD4 mov r14d,ebx
140780FD7 and r14d,100h
140780FDE mov r13d,r14d
140780FE1 shr r13d,8
140780FE5 cmp byte ptr [rbp+318h],0
140780FEC je 000000014078102A
140780FEE mov eax,4
140780FF3 mov qword ptr [rbp+208h],rax
140780FFA xor r14d,r14d
140780FFD test r13b,r13b
140781000 mov qword ptr [rbp+118h],r15
140781007 mov dword ptr [rbp+1D8h],r13d
14078100E je 00000001407810CE
140781014 test bl,1
140781017 je 00000001407810D6
14078101D movss xmm1,dword ptr [__real@404ccccd]
140781025 jmp 00000001407810DE  ; @0x1407810DE
14078102A mov r12,r15
14078102D mov rax,qword ptr [rbp+338h]
140781034 mov rcx,qword ptr [rbp+330h]
14078103B mov rdx,qword ptr [rbp+328h]
140781042 mov r8,qword ptr [rbp+310h]
140781049 mov qword ptr [rbp+120h],rdx
140781050 mov qword ptr [rbp+128h],rcx
140781057 mov qword ptr [rbp+130h],rax
14078105E mov qword ptr [rbp+138h],r8
140781065 mov edi,dword ptr [rsi]
140781067 mov esi,dword ptr [rsi+4]
14078106A mov byte ptr [rbp+217h],1
140781071 lea rcx,[rbp+120h]
140781078 mov edx,edi
14078107A mov r8d,esi
14078107D call 0000000140782F50  ; @0x140782F50
140781082 test al,al
140781084 je 0000000140781784
14078108A mov byte ptr [rbp+217h],1
140781091 lea rcx,[rbp+120h]
140781098 mov edx,esi
14078109A mov r8d,edi
14078109D call 0000000140782F50  ; @0x140782F50
1407810A2 movzx ecx,r13b
1407810A6 test al,al
1407810A8 mov r13d,1
1407810AE cmove r13d,ecx
1407810B2 test r13b,r13b
1407810B5 jne 00000001407817B0
1407810BB movss xmm8,dword ptr [__real@3f666666]
1407810C4 subss xmm8,xmm6
1407810C9 jmp 00000001407817B9  ; @0x1407817B9
1407810CE xorps xmm1,xmm1
1407810D1 test bl,1
1407810D4 je 00000001407810DE
1407810D6 movss xmm1,dword ptr [__real@3fcccccd]
1407810DE movss xmm0,dword ptr [rbp+30h]
1407810E3 subss xmm0,xmm1
1407810E7 mulss xmm0,dword ptr [__real@3e800000]
1407810EF call roundf  ; @0x142923FA0
1407810F4 cvttss2si rax,xmm0
1407810F9 mov rcx,rax
1407810FC sar rcx,3Fh
140781100 movaps xmm1,xmm0
140781103 subss xmm1,dword ptr [__real@5f000000]
14078110B cvttss2si rdx,xmm1
140781110 and rdx,rcx
140781113 or rdx,rax
140781116 xor esi,esi
140781118 xorps xmm8,xmm8
14078111C ucomiss xmm0,xmm8
140781120 mov eax,0
140781125 cmovae rax,rdx
140781129 ucomiss xmm0,dword ptr [__real@5f7fffff]
140781130 mov rcx,0FFFFFFFFFFFFFFFFh
140781137 cmovbe rcx,rax
14078113B cmp rcx,1
14078113F adc rcx,0
140781143 mov qword ptr [rbp+38h],rcx
140781147 mov qword ptr [rbp+0C0h],0
