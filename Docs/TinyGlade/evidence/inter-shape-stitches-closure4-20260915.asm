; Local binary: D:/MyProject/Tiny Glade/tiny-glade.exe (same build as inter-shape-stitches-20260915.asm)
; dumpbin /DISASM:NOBYTES /RANGE:0x140EDBB60,0x140EDBF60 + tiny_glade.pdb (2026-09-15).
; detect_intra_shape_corners::closure$4 = the per-crossing callback intersect_shapes calls; it writes Corner (64 B) and ShapeIntersections[(min,max) wall pair].
system_clutter::inter_shape_stitches::detect_intra_shape_corners::detect_intra_shape_corners::closure$4:
  0000000140EDBB60: push        r15
  0000000140EDBB62: push        r14
  0000000140EDBB64: push        r13
  0000000140EDBB66: push        r12
  0000000140EDBB68: push        rsi
  0000000140EDBB69: push        rdi
  0000000140EDBB6A: push        rbp
  0000000140EDBB6B: push        rbx
  0000000140EDBB6C: sub         rsp,0E8h
  0000000140EDBB73: movaps      xmmword ptr [rsp+0D0h],xmm11
  0000000140EDBB7C: movaps      xmmword ptr [rsp+0C0h],xmm10
  0000000140EDBB85: movaps      xmmword ptr [rsp+0B0h],xmm9
  0000000140EDBB8E: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000140EDBB97: movaps      xmmword ptr [rsp+90h],xmm7
  0000000140EDBB9F: movaps      xmmword ptr [rsp+80h],xmm6
  0000000140EDBBA7: mov         rbx,r9
  0000000140EDBBAA: movaps      xmm6,xmm2
  0000000140EDBBAD: movaps      xmm7,xmm1
  0000000140EDBBB0: mov         rsi,rcx
  0000000140EDBBB3: mov         rax,qword ptr [rcx]
  0000000140EDBBB6: mov         rcx,qword ptr [rax]
  0000000140EDBBB9: call        _ZN12country_core9resources14terrain_height28TerrainHeightsData$LT$Ty$GT$18sample_at_world_xz17h51d12d20427988f4E
  0000000140EDBBBE: movaps      xmm8,xmm0
  0000000140EDBBC2: mov         rdi,qword ptr [rsi+8]
  0000000140EDBBC6: lea         rcx,[rdi+88h]
  0000000140EDBBCD: movaps      xmm1,xmm7
  0000000140EDBBD0: movaps      xmm2,xmm6
  0000000140EDBBD3: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$25get_approx_coord_from_pos17h09ac8350f46460dbE
  0000000140EDBBD8: mov         rdx,qword ptr [rdi+100h]
  0000000140EDBBDF: mov         ecx,eax
  0000000140EDBBE1: cmp         rdx,rcx
  0000000140EDBBE4: jbe         0000000140EDBF26
  0000000140EDBBEA: lea         rax,[rcx+1]
  0000000140EDBBEE: cmp         rax,rdx
  0000000140EDBBF1: jae         0000000140EDBF32
  0000000140EDBBF7: mov         rax,qword ptr [rdi+0F8h]
  0000000140EDBBFE: movss       xmm1,dword ptr [rax+rcx*4]
  0000000140EDBC03: movss       xmm2,dword ptr [rax+rcx*4+4]
  0000000140EDBC09: subss       xmm2,xmm1
  0000000140EDBC0D: mulss       xmm0,xmm2
  0000000140EDBC11: addss       xmm0,xmm1
  0000000140EDBC15: movss       xmm1,dword ptr [__real@3eeb851f]
  0000000140EDBC1D: addss       xmm1,xmm0
  0000000140EDBC21: xorps       xmm9,xmm9
  0000000140EDBC25: cmpltss     xmm9,xmm0
  0000000140EDBC2B: andps       xmm1,xmm9
  0000000140EDBC2F: andnps      xmm9,xmm0
  0000000140EDBC33: orps        xmm9,xmm1
  0000000140EDBC37: mov         rax,qword ptr [rsi+10h]
  0000000140EDBC3B: mov         rcx,qword ptr [rsi+18h]
  0000000140EDBC3F: addss       xmm9,dword ptr [rax]
  0000000140EDBC44: mov         rax,qword ptr [rsi+20h]
  0000000140EDBC48: movss       xmm0,dword ptr [rax]
  0000000140EDBC4C: movaps      xmm1,xmm0
  0000000140EDBC4F: maxss       xmm1,xmm9
  0000000140EDBC54: cmpunordss  xmm9,xmm9
  0000000140EDBC5A: movaps      xmm2,xmm9
  0000000140EDBC5E: andnps      xmm2,xmm1
  0000000140EDBC61: andps       xmm9,xmm0
  0000000140EDBC65: orps        xmm9,xmm2
  0000000140EDBC69: movaps      xmm0,xmm8
  0000000140EDBC6D: maxss       xmm0,xmm9
  0000000140EDBC72: cmpunordss  xmm9,xmm9
  0000000140EDBC78: movaps      xmm1,xmm9
  0000000140EDBC7C: andnps      xmm1,xmm0
  0000000140EDBC7F: andps       xmm9,xmm8
  0000000140EDBC83: orps        xmm9,xmm1
  0000000140EDBC87: movss       xmm0,dword ptr [rcx]
  0000000140EDBC8B: mov         rax,qword ptr [rsi+28h]
  0000000140EDBC8F: movss       xmm1,dword ptr [rax]
  0000000140EDBC93: movaps      xmm10,xmm0
  0000000140EDBC97: cmpunordss  xmm10,xmm0
  0000000140EDBC9D: movaps      xmm2,xmm10
  0000000140EDBCA1: andps       xmm2,xmm1
  0000000140EDBCA4: minss       xmm1,xmm0
  0000000140EDBCA8: andnps      xmm10,xmm1
  0000000140EDBCAC: orps        xmm10,xmm2
  0000000140EDBCB0: ucomiss     xmm9,xmm10
  0000000140EDBCB4: jae         0000000140EDBEDE
  0000000140EDBCBA: mov         rdi,rbx
  0000000140EDBCBD: movss       xmm11,dword ptr [rsp+158h]
  0000000140EDBCC7: mov         edx,dword ptr [rsp+150h]
  0000000140EDBCCE: mov         rbx,qword ptr [rsi+30h]
  0000000140EDBCD2: mov         r14,qword ptr [rsi+38h]
  0000000140EDBCD6: mov         r13,qword ptr [r14]
  0000000140EDBCD9: mov         r15,qword ptr [rsi+40h]
  0000000140EDBCDD: mov         rbp,qword ptr [r15]
  0000000140EDBCE0: mov         r12,qword ptr [rbx+10h]
  0000000140EDBCE4: cmp         r12,qword ptr [rbx]
  0000000140EDBCE7: jne         0000000140EDBCFF
  0000000140EDBCE9: lea         rdx,[142B95E78h]
  0000000140EDBCF0: mov         rcx,rbx
  0000000140EDBCF3: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0217977ba397d77cE
  0000000140EDBCF8: mov         edx,dword ptr [rsp+150h]
  0000000140EDBCFF: mov         rax,qword ptr [rbx+8]
  0000000140EDBD03: mov         rcx,r12
  0000000140EDBD06: shl         rcx,6
  0000000140EDBD0A: mov         dword ptr [rax+rcx],edx
  0000000140EDBD0D: movss       dword ptr [rax+rcx+4],xmm11
  0000000140EDBD14: movss       dword ptr [rax+rcx+8],xmm7
  0000000140EDBD1A: movss       dword ptr [rax+rcx+0Ch],xmm6
  0000000140EDBD20: movss       dword ptr [rax+rcx+10h],xmm9
  0000000140EDBD27: movss       dword ptr [rax+rcx+14h],xmm10
  0000000140EDBD2E: movdqu      xmm0,xmmword ptr [rdi]
  0000000140EDBD32: movdqu      xmmword ptr [rax+rcx+18h],xmm0
  0000000140EDBD38: mov         qword ptr [rax+rcx+28h],r13
  0000000140EDBD3D: mov         qword ptr [rax+rcx+30h],rbp
  0000000140EDBD42: movss       dword ptr [rax+rcx+38h],xmm8
  0000000140EDBD49: inc         r12
  0000000140EDBD4C: mov         qword ptr [rbx+10h],r12
  0000000140EDBD50: mov         rdi,qword ptr [r14]
  0000000140EDBD53: mov         rax,qword ptr [r15]
  0000000140EDBD56: cmp         rax,rdi
  0000000140EDBD59: mov         rbx,rdi
  0000000140EDBD5C: cmovb       rbx,rax
  0000000140EDBD60: cmova       rdi,rax
  0000000140EDBD64: mov         qword ptr [rsp+28h],rbx
  0000000140EDBD69: mov         qword ptr [rsp+30h],rdi
  0000000140EDBD6E: mov         rsi,qword ptr [rsi+48h]
  0000000140EDBD72: cmp         qword ptr [rsi+18h],0
  0000000140EDBD77: je          0000000140EDBE1F
  0000000140EDBD7D: lea         rcx,[rsi+20h]
  0000000140EDBD81: lea         rdx,[rsp+28h]
  0000000140EDBD86: call        _ZN4core4hash11BuildHasher8hash_one17h4a0a715699493c0bE
  0000000140EDBD8B: mov         rcx,qword ptr [rsi]
  0000000140EDBD8E: mov         rdx,qword ptr [rsi+8]
  0000000140EDBD92: mov         r8,rdx
  0000000140EDBD95: and         r8,rax
  0000000140EDBD98: shr         rax,39h
  0000000140EDBD9C: movd        xmm0,eax
  0000000140EDBDA0: punpcklbw   xmm0,xmm0
  0000000140EDBDA4: pshuflw     xmm0,xmm0,0
  0000000140EDBDA9: pshufd      xmm0,xmm0,0
  0000000140EDBDAE: lea         rax,[rcx-28h]
  0000000140EDBDB2: xor         r9d,r9d
  0000000140EDBDB5: pcmpeqd     xmm1,xmm1
  0000000140EDBDB9: movdqu      xmm2,xmmword ptr [rcx+r8]
  0000000140EDBDBF: movdqa      xmm3,xmm2
  0000000140EDBDC3: pcmpeqb     xmm3,xmm0
  0000000140EDBDC7: pmovmskb    r10d,xmm3
  0000000140EDBDCC: test        r10d,r10d
  0000000140EDBDCF: je          0000000140EDBE01
  0000000140EDBDD1: tzcnt       r11d,r10d
  0000000140EDBDD6: add         r11,r8
  0000000140EDBDD9: and         r11,rdx
  0000000140EDBDDC: neg         r11
  0000000140EDBDDF: lea         r11,[r11+r11*4]
  0000000140EDBDE3: cmp         rbx,qword ptr [rax+r11*8]
  0000000140EDBDE7: jne         0000000140EDBDF4
  0000000140EDBDE9: cmp         rdi,qword ptr [rax+r11*8+8]
  0000000140EDBDEE: je          0000000140EDBEA4
  0000000140EDBDF4: lea         r11d,[r10-1]
  0000000140EDBDF8: and         r11w,r10w
  0000000140EDBDFC: mov         r10d,r11d
  0000000140EDBDFF: jne         0000000140EDBDD1
  0000000140EDBE01: pcmpeqb     xmm2,xmm1
  0000000140EDBE05: pmovmskb    r10d,xmm2
  0000000140EDBE0A: test        r10d,r10d
  0000000140EDBE0D: jne         0000000140EDBE1F
  0000000140EDBE0F: add         r8,r9
  0000000140EDBE12: add         r8,10h
  0000000140EDBE16: add         r9,10h
  0000000140EDBE1A: and         r8,rdx
  0000000140EDBE1D: jmp         0000000140EDBDB9
  0000000140EDBE1F: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  0000000140EDBE26: mov         ecx,8
  0000000140EDBE2B: mov         edx,4
  0000000140EDBE30: call        __rust_alloc
  0000000140EDBE35: test        rax,rax
  0000000140EDBE38: je          0000000140EDBF41
  0000000140EDBE3E: movss       dword ptr [rax],xmm7
  0000000140EDBE42: movss       dword ptr [rax+4],xmm6
  0000000140EDBE47: mov         qword ptr [rsp+38h],1
  0000000140EDBE50: mov         qword ptr [rsp+40h],rax
  0000000140EDBE55: mov         qword ptr [rsp+48h],1
  0000000140EDBE5E: movups      xmm0,xmmword ptr [rsp+28h]
  0000000140EDBE63: movaps      xmmword ptr [rsp+50h],xmm0
  0000000140EDBE68: lea         rcx,[rsp+68h]
  0000000140EDBE6D: lea         r8,[rsp+50h]
  0000000140EDBE72: lea         r9,[rsp+38h]
  0000000140EDBE77: mov         rdx,rsi
  0000000140EDBE7A: call        _ZN9hashbrown3map28HashMap$LT$K$C$V$C$S$C$A$GT$6insert17h8c5da8b7465d21e5E
  0000000140EDBE7F: mov         rdx,qword ptr [rsp+68h]
  0000000140EDBE84: mov         rax,rdx
  0000000140EDBE87: neg         rax
  0000000140EDBE8A: jo          0000000140EDBEDE
  0000000140EDBE8C: jae         0000000140EDBEDE
  0000000140EDBE8E: mov         rcx,qword ptr [rsp+70h]
  0000000140EDBE93: shl         rdx,3
  0000000140EDBE97: mov         r8d,4
  0000000140EDBE9D: call        __rust_dealloc
  0000000140EDBEA2: jmp         0000000140EDBEDE
  0000000140EDBEA4: lea         rsi,[rcx+r11*8]
  0000000140EDBEA8: mov         rdi,qword ptr [rcx+r11*8-8]
  0000000140EDBEAD: cmp         rdi,qword ptr [rcx+r11*8-18h]
  0000000140EDBEB2: jne         0000000140EDBEC8
  0000000140EDBEB4: lea         rcx,[rcx+r11*8]
  0000000140EDBEB8: add         rcx,0FFFFFFFFFFFFFFE8h
  0000000140EDBEBC: lea         rdx,[142B95E90h]
  0000000140EDBEC3: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h09b7af127c6b1b53E
  0000000140EDBEC8: mov         rax,qword ptr [rsi-10h]
  0000000140EDBECC: movss       dword ptr [rax+rdi*8],xmm7
  0000000140EDBED1: movss       dword ptr [rax+rdi*8+4],xmm6
  0000000140EDBED7: inc         rdi
  0000000140EDBEDA: mov         qword ptr [rsi-8],rdi
  0000000140EDBEDE: movaps      xmm6,xmmword ptr [rsp+80h]
  0000000140EDBEE6: movaps      xmm7,xmmword ptr [rsp+90h]
  0000000140EDBEEE: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000140EDBEF7: movaps      xmm9,xmmword ptr [rsp+0B0h]
  0000000140EDBF00: movaps      xmm10,xmmword ptr [rsp+0C0h]
  0000000140EDBF09: movaps      xmm11,xmmword ptr [rsp+0D0h]
  0000000140EDBF12: add         rsp,0E8h
  0000000140EDBF19: pop         rbx
  0000000140EDBF1A: pop         rbp
  0000000140EDBF1B: pop         rdi
  0000000140EDBF1C: pop         rsi
  0000000140EDBF1D: pop         r12
  0000000140EDBF1F: pop         r13
  0000000140EDBF21: pop         r14
  0000000140EDBF23: pop         r15
  0000000140EDBF25: ret
  0000000140EDBF26: lea         r8,[142B95E60h]
  0000000140EDBF2D: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140EDBF32: lea         r8,[142B95E60h]
  0000000140EDBF39: mov         rcx,rax
  0000000140EDBF3C: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140EDBF41: mov         ecx,4
  0000000140EDBF46: mov         edx,8
  0000000140EDBF4B: call        _ZN5alloc5alloc18handle_alloc_error17h3e7daf9bcd04547aE
  0000000140EDBF50: int         3
  0000000140EDBF51: CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC     ...............
