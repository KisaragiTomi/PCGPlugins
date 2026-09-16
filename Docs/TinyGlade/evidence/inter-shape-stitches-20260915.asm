; Local binary: D:/MyProject/Tiny Glade/tiny-glade.exe
; SHA-256: cf048e27acdc266b2efc7c094dd435bacecb3f26c2afe54de4d09cd3f7036323
; dumpbin /DISASM:NOBYTES + tiny_glade.pdb, filtered to system_clutter::inter_shape_stitches::* (2026-09-15).
; Functions: detect_intra_shape_corners, intersect_shapes (x2 mono), add_hole_at_shape_intersection, WallAndRoofChanges::dirty_walls, stitch_bricks::{spawn_stitches, spawn_stitch_bricks}.

_ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners16intersect_shapes17h25a4dbb3a7621abfE:
  0000000140EDBF60: push        r15
  0000000140EDBF62: push        r14
  0000000140EDBF64: push        r13
  0000000140EDBF66: push        r12
  0000000140EDBF68: push        rsi
  0000000140EDBF69: push        rdi
  0000000140EDBF6A: push        rbp
  0000000140EDBF6B: push        rbx
  0000000140EDBF6C: sub         rsp,2B8h
  0000000140EDBF73: movaps      xmmword ptr [rsp+2A0h],xmm15
  0000000140EDBF7C: movaps      xmmword ptr [rsp+290h],xmm14
  0000000140EDBF85: movaps      xmmword ptr [rsp+280h],xmm13
  0000000140EDBF8E: movaps      xmmword ptr [rsp+270h],xmm12
  0000000140EDBF97: movaps      xmmword ptr [rsp+260h],xmm11
  0000000140EDBFA0: movaps      xmmword ptr [rsp+250h],xmm10
  0000000140EDBFA9: movaps      xmmword ptr [rsp+240h],xmm9
  0000000140EDBFB2: movaps      xmmword ptr [rsp+230h],xmm8
  0000000140EDBFBB: movaps      xmmword ptr [rsp+220h],xmm7
  0000000140EDBFC3: movaps      xmmword ptr [rsp+210h],xmm6
  0000000140EDBFCB: mov         esi,r9d
  0000000140EDBFCE: mov         qword ptr [rsp+70h],r8
  0000000140EDBFD3: mov         r14,rdx
  0000000140EDBFD6: mov         r15,rcx
  0000000140EDBFD9: mov         rbx,qword ptr [rsp+320h]
  0000000140EDBFE1: mov         edi,dword ptr [rcx]
  0000000140EDBFE3: movzx       eax,byte ptr [rdx]
  0000000140EDBFE6: test        edi,edi
  0000000140EDBFE8: mov         byte ptr [rsp+3Fh],r9b
  0000000140EDBFED: je          0000000140EDC17D
  0000000140EDBFF3: test        al,1
  0000000140EDBFF5: je          0000000140EDC18A
  0000000140EDBFFB: add         r15,4
  0000000140EDBFFF: add         r14,4
  0000000140EDC003: lea         rcx,[rsp+0B0h]
  0000000140EDC00B: mov         rdx,r15
  0000000140EDC00E: call        _ZN5utils8geometry9rectangle11Rectangle2d26as_transformation_and_size17h6846edc8a380f996E
  0000000140EDC013: movaps      xmm9,xmmword ptr [rsp+0B0h]
  0000000140EDC01C: movsd       xmm10,mmword ptr [rsp+0C0h]
  0000000140EDC026: movsd       xmm11,mmword ptr [rsp+0D0h]
  0000000140EDC030: movshdup    xmm8,xmm11
  0000000140EDC035: lea         rcx,[rsp+0B0h]
  0000000140EDC03D: mov         rdx,r14
  0000000140EDC040: call        _ZN5utils8geometry9rectangle11Rectangle2d26as_transformation_and_size17h6846edc8a380f996E
  0000000140EDC045: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000140EDC04D: movsd       xmm6,mmword ptr [rsp+0C0h]
  0000000140EDC056: unpcklps    xmm6,xmm6
  0000000140EDC059: movss       dword ptr [rsp+20h],xmm8
  0000000140EDC060: lea         rcx,[rsp+200h]
  0000000140EDC068: xorps       xmm1,xmm1
  0000000140EDC06B: xorps       xmm2,xmm2
  0000000140EDC06E: movaps      xmmword ptr [rsp+190h],xmm11
  0000000140EDC077: movaps      xmm3,xmm11
  0000000140EDC07B: call        _ZN5utils8geometry4aabb5Aabb216from_center_dims17h1107e02350cfd9a4E
  0000000140EDC080: movaps      xmm0,xmm9
  0000000140EDC084: shufps      xmm0,xmm9,0EBh
  0000000140EDC089: mulps       xmm0,xmm9
  0000000140EDC08D: movshdup    xmm1,xmm0
  0000000140EDC091: subss       xmm0,xmm1
  0000000140EDC095: shufps      xmm0,xmm0,0
  0000000140EDC099: movaps      xmm1,xmmword ptr [__xmm@3f800000bf800000bf8000003f800000]
  0000000140EDC0A0: divps       xmm1,xmm0
  0000000140EDC0A3: movaps      xmmword ptr [rsp+80h],xmm9
  0000000140EDC0AC: movaps      xmm0,xmm9
  0000000140EDC0B0: shufps      xmm0,xmm9,27h
  0000000140EDC0B5: mulps       xmm0,xmm1
  0000000140EDC0B8: movaps      xmmword ptr [rsp+1A0h],xmm10
  0000000140EDC0C1: movaps      xmm1,xmm10
  0000000140EDC0C5: unpcklps    xmm1,xmm10
  0000000140EDC0C9: mulps       xmm1,xmm0
  0000000140EDC0CC: movaps      xmm2,xmm1
  0000000140EDC0CF: unpckhpd    xmm2,xmm1
  0000000140EDC0D3: addps       xmm2,xmm1
  0000000140EDC0D6: movaps      xmm1,xmm7
  0000000140EDC0D9: unpcklps    xmm1,xmm7
  0000000140EDC0DC: unpckhps    xmm7,xmm7
  0000000140EDC0DF: mulps       xmm1,xmm0
  0000000140EDC0E2: mulps       xmm7,xmm0
  0000000140EDC0E5: movaps      xmm3,xmm1
  0000000140EDC0E8: unpckhpd    xmm3,xmm7
  0000000140EDC0EC: movlhps     xmm1,xmm7
  0000000140EDC0EF: addps       xmm1,xmm3
  0000000140EDC0F2: mulps       xmm0,xmm6
  0000000140EDC0F5: movaps      xmm3,xmm0
  0000000140EDC0F8: unpckhpd    xmm3,xmm0
  0000000140EDC0FC: addps       xmm3,xmm0
  0000000140EDC0FF: subps       xmm3,xmm2
  0000000140EDC102: movaps      xmmword ptr [rsp+90h],xmm1
  0000000140EDC10A: movlps      qword ptr [rsp+0A0h],xmm3
  0000000140EDC112: lea         r12,[rsp+1C0h]
  0000000140EDC11A: mov         rcx,r12
  0000000140EDC11D: mov         rdx,r14
  0000000140EDC120: call        _ZN5utils8geometry9rectangle11Rectangle2d27as_points2_centered_aligned17h13f0455834b1169cE
  0000000140EDC125: mov         qword ptr [rsp+1E0h],0
  0000000140EDC131: mov         qword ptr [rsp+1E8h],4
  0000000140EDC13D: lea         rax,[rsp+90h]
  0000000140EDC145: mov         qword ptr [rsp+1B8h],rax
  0000000140EDC14D: xorps       xmm0,xmm0
  0000000140EDC150: movups      xmmword ptr [rsp+180h],xmm0
  0000000140EDC158: xor         eax,eax
  0000000140EDC15A: test        al,al
  0000000140EDC15C: jne         0000000140EDC8FD
  0000000140EDC162: mov         eax,4
  0000000140EDC167: mov         ecx,4
  0000000140EDC16C: cmp         ecx,1
  0000000140EDC16F: jne         0000000140EDC863
  0000000140EDC175: xor         r9d,r9d
  0000000140EDC178: jmp         0000000140EDC8D5
  0000000140EDC17D: test        al,1
  0000000140EDC17F: je          0000000140EDC601
  0000000140EDC185: mov         r12,r15
  0000000140EDC188: jmp         0000000140EDC190
  0000000140EDC18A: mov         r12,r14
  0000000140EDC18D: mov         r14,r15
  0000000140EDC190: add         r14,4
  0000000140EDC194: lea         rcx,[rsp+40h]
  0000000140EDC199: mov         rdx,r14
  0000000140EDC19C: call        _ZN5utils8geometry9rectangle11Rectangle2d10as_points217h4b26c106db0a5c9aE
  0000000140EDC1A1: mov         qword ptr [rsp+60h],0
  0000000140EDC1AA: mov         qword ptr [rsp+68h],4
  0000000140EDC1B3: movaps      xmm0,xmmword ptr [rsp+50h]
  0000000140EDC1B8: movaps      xmm1,xmmword ptr [rsp+40h]
  0000000140EDC1BD: movups      xmmword ptr [rsp+0E8h],xmm1
  0000000140EDC1C5: movaps      xmm1,xmmword ptr [rsp+40h]
  0000000140EDC1CA: movaps      xmm2,xmmword ptr [rsp+50h]
  0000000140EDC1CF: movups      xmmword ptr [rsp+0F8h],xmm2
  0000000140EDC1D7: mov         rax,qword ptr [rsp+60h]
  0000000140EDC1DC: mov         qword ptr [rsp+108h],rax
  0000000140EDC1E4: mov         rax,qword ptr [rsp+68h]
  0000000140EDC1E9: mov         qword ptr [rsp+110h],rax
  0000000140EDC1F1: movups      xmmword ptr [rsp+0B8h],xmm1
  0000000140EDC1F9: movups      xmmword ptr [rsp+0C8h],xmm0
  0000000140EDC201: mov         qword ptr [rsp+0D8h],0
  0000000140EDC20D: mov         qword ptr [rsp+0E0h],4
  0000000140EDC219: mov         dword ptr [rsp+118h],0
  0000000140EDC224: mov         qword ptr [rsp+0B0h],4
  0000000140EDC230: lea         rcx,[rsp+90h]
  0000000140EDC238: lea         rdx,[rsp+0B0h]
  0000000140EDC240: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h64c1a140eb3a9994E
  0000000140EDC245: cmp         dword ptr [rsp+90h],1
  0000000140EDC24D: jne         0000000140EDD1D6
  0000000140EDC253: add         r12,4
  0000000140EDC257: mov         eax,edi
  0000000140EDC259: xor         eax,1
  0000000140EDC25C: mov         dword ptr [rsp+80h],eax
  0000000140EDC263: lea         r15,[rsp+40h]
  0000000140EDC268: movss       xmm7,dword ptr [__real@3f800000]
  0000000140EDC270: xorps       xmm8,xmm8
  0000000140EDC274: movaps      xmm9,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDC27C: lea         r13,[rsp+90h]
  0000000140EDC284: lea         rbp,[rsp+0B0h]
  0000000140EDC28C: jmp         0000000140EDC402
  0000000140EDC291: mov         rax,qword ptr [rbx+8]
  0000000140EDC295: lea         rcx,[rsi+rsi*2]
  0000000140EDC299: mov         dword ptr [rax+rcx*4],edi
  0000000140EDC29C: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDC2A1: inc         rsi
  0000000140EDC2A4: mov         qword ptr [rbx+10h],rsi
  0000000140EDC2A8: movzx       esi,byte ptr [rsp+3Fh]
  0000000140EDC2AD: movsd       xmm0,mmword ptr [r12]
  0000000140EDC2B3: movaps      xmm12,xmm6
  0000000140EDC2B7: subps       xmm12,xmm0
  0000000140EDC2BB: movaps      xmm0,xmm12
  0000000140EDC2BF: mulps       xmm0,xmm12
  0000000140EDC2C3: movshdup    xmm1,xmm0
  0000000140EDC2C7: addss       xmm1,xmm0
  0000000140EDC2CB: xorps       xmm0,xmm0
  0000000140EDC2CE: sqrtss      xmm0,xmm1
  0000000140EDC2D2: movaps      xmm14,xmm7
  0000000140EDC2D6: divss       xmm14,xmm0
  0000000140EDC2DB: movaps      xmm13,xmm14
  0000000140EDC2DF: xorps       xmm13,xmm9
  0000000140EDC2E3: mov         rcx,r14
  0000000140EDC2E6: xor         edx,edx
  0000000140EDC2E8: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDC2ED: unpcklps    xmm0,xmm1
  0000000140EDC2F0: movaps      xmm1,xmm6
  0000000140EDC2F3: subps       xmm1,xmm0
  0000000140EDC2F6: mulps       xmm1,xmm1
  0000000140EDC2F9: movshdup    xmm0,xmm1
  0000000140EDC2FD: addss       xmm0,xmm1
  0000000140EDC301: sqrtss      xmm15,xmm0
  0000000140EDC306: minss       xmm15,dword ptr [__real@42c80000]
  0000000140EDC30F: mov         edx,1
  0000000140EDC314: mov         rcx,r14
  0000000140EDC317: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDC31C: unpcklps    xmm0,xmm1
  0000000140EDC31F: movaps      xmm1,xmm6
  0000000140EDC322: subps       xmm1,xmm0
  0000000140EDC325: mulps       xmm1,xmm1
  0000000140EDC328: movshdup    xmm0,xmm1
  0000000140EDC32C: addss       xmm0,xmm1
  0000000140EDC330: sqrtss      xmm10,xmm0
  0000000140EDC335: minss       xmm10,xmm15
  0000000140EDC33A: mov         edx,2
  0000000140EDC33F: mov         rcx,r14
  0000000140EDC342: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDC347: unpcklps    xmm0,xmm1
  0000000140EDC34A: movaps      xmm1,xmm6
  0000000140EDC34D: subps       xmm1,xmm0
  0000000140EDC350: mulps       xmm1,xmm1
  0000000140EDC353: movshdup    xmm0,xmm1
  0000000140EDC357: addss       xmm0,xmm1
  0000000140EDC35B: xorps       xmm15,xmm15
  0000000140EDC35F: sqrtss      xmm15,xmm0
  0000000140EDC364: minss       xmm15,xmm10
  0000000140EDC369: mov         edx,3
  0000000140EDC36E: mov         rcx,r14
  0000000140EDC371: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDC376: unpcklps    xmm0,xmm1
  0000000140EDC379: movaps      xmm1,xmm6
  0000000140EDC37C: subps       xmm1,xmm0
  0000000140EDC37F: mulps       xmm1,xmm1
  0000000140EDC382: movshdup    xmm0,xmm1
  0000000140EDC386: addss       xmm0,xmm1
  0000000140EDC38A: sqrtss      xmm0,xmm0
  0000000140EDC38E: minss       xmm0,xmm15
  0000000140EDC393: unpcklps    xmm13,xmm14
  0000000140EDC397: shufps      xmm12,xmm12,0E1h
  0000000140EDC39C: mulps       xmm12,xmm13
  0000000140EDC3A0: movlps      qword ptr [rsp+40h],xmm12
  0000000140EDC3A6: movlhps     xmm11,xmm6
  0000000140EDC3AA: movups      xmmword ptr [rsp+48h],xmm11
  0000000140EDC3B0: mov         qword ptr [rsp+58h],1
  0000000140EDC3B9: mov         dword ptr [rsp+60h],1
  0000000140EDC3C1: movss       dword ptr [rsp+64h],xmm0
  0000000140EDC3C7: movshdup    xmm2,xmm6
  0000000140EDC3CB: movss       dword ptr [rsp+28h],xmm0
  0000000140EDC3D1: mov         dword ptr [rsp+20h],1
  0000000140EDC3D9: mov         rcx,qword ptr [rsp+70h]
  0000000140EDC3DE: movaps      xmm1,xmm6
  0000000140EDC3E1: mov         r9,r15
  0000000140EDC3E4: call        0000000140EDBB60
  0000000140EDC3E9: mov         rcx,r13
  0000000140EDC3EC: mov         rdx,rbp
  0000000140EDC3EF: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h64c1a140eb3a9994E
  0000000140EDC3F4: test        byte ptr [rsp+90h],1
  0000000140EDC3FC: je          0000000140EDD1D6
  0000000140EDC402: movsd       xmm6,mmword ptr [rsp+94h]
  0000000140EDC40B: movsd       xmm14,mmword ptr [rsp+9Ch]
  0000000140EDC415: movshdup    xmm3,xmm6
  0000000140EDC419: movaps      xmm12,xmm14
  0000000140EDC41D: subps       xmm12,xmm6
  0000000140EDC421: movshdup    xmm0,xmm12
  0000000140EDC426: movss       dword ptr [rsp+20h],xmm12
  0000000140EDC42D: movss       dword ptr [rsp+28h],xmm0
  0000000140EDC433: mov         rcx,r15
  0000000140EDC436: mov         rdx,r12
  0000000140EDC439: movaps      xmm2,xmm6
  0000000140EDC43C: call        _ZN5utils8geometry6circle8Circle2d13intersect_ray17h7d4e0430dcf54c87E
  0000000140EDC441: test        byte ptr [rsp+40h],1
  0000000140EDC446: je          0000000140EDC3E9
  0000000140EDC448: movss       xmm0,dword ptr [rsp+44h]
  0000000140EDC44E: movss       xmm13,dword ptr [rsp+48h]
  0000000140EDC455: movaps      xmm1,xmm12
  0000000140EDC459: mulps       xmm1,xmm12
  0000000140EDC45D: movshdup    xmm2,xmm1
  0000000140EDC461: addss       xmm2,xmm1
  0000000140EDC465: xorps       xmm1,xmm1
  0000000140EDC468: sqrtss      xmm1,xmm2
  0000000140EDC46C: movaps      xmm2,xmm7
  0000000140EDC46F: divss       xmm2,xmm1
  0000000140EDC473: movsldup    xmm11,xmm2
  0000000140EDC478: mulps       xmm11,xmm12
  0000000140EDC47C: ucomiss     xmm0,xmm8
  0000000140EDC480: jb          0000000140EDC5AE
  0000000140EDC486: ucomiss     xmm7,xmm0
  0000000140EDC489: jb          0000000140EDC5AE
  0000000140EDC48F: movsldup    xmm1,xmm0
  0000000140EDC493: mulps       xmm1,xmm12
  0000000140EDC497: addps       xmm1,xmm6
  0000000140EDC49A: test        sil,sil
  0000000140EDC49D: je          0000000140EDC4EA
  0000000140EDC49F: mov         rbx,qword ptr [rsp+320h]
  0000000140EDC4A7: mov         rsi,qword ptr [rbx+10h]
  0000000140EDC4AB: cmp         rsi,qword ptr [rbx]
  0000000140EDC4AE: jne         0000000140EDC4C7
  0000000140EDC4B0: mov         rcx,rbx
  0000000140EDC4B3: lea         rdx,[142B95ED8h]
  0000000140EDC4BA: movaps      xmm15,xmm1
  0000000140EDC4BE: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDC4C3: movaps      xmm1,xmm15
  0000000140EDC4C7: mov         rax,qword ptr [rbx+8]
  0000000140EDC4CB: lea         rcx,[rsi+rsi*2]
  0000000140EDC4CF: mov         edx,dword ptr [rsp+80h]
  0000000140EDC4D6: mov         dword ptr [rax+rcx*4],edx
  0000000140EDC4D9: movlps      qword ptr [rax+rcx*4+4],xmm1
  0000000140EDC4DE: inc         rsi
  0000000140EDC4E1: mov         qword ptr [rbx+10h],rsi
  0000000140EDC4E5: movzx       esi,byte ptr [rsp+3Fh]
  0000000140EDC4EA: movaps      xmm0,xmm1
  0000000140EDC4ED: subps       xmm0,xmm6
  0000000140EDC4F0: mulps       xmm0,xmm0
  0000000140EDC4F3: movshdup    xmm2,xmm0
  0000000140EDC4F7: addss       xmm2,xmm0
  0000000140EDC4FB: sqrtss      xmm2,xmm2
  0000000140EDC4FF: movaps      xmm0,xmm1
  0000000140EDC502: subps       xmm0,xmm14
  0000000140EDC506: mulps       xmm0,xmm0
  0000000140EDC509: movshdup    xmm3,xmm0
  0000000140EDC50D: addss       xmm3,xmm0
  0000000140EDC511: sqrtss      xmm3,xmm3
  0000000140EDC515: movaps      xmm0,xmm2
  0000000140EDC518: cmpunordss  xmm0,xmm2
  0000000140EDC51D: movaps      xmm4,xmm0
  0000000140EDC520: andps       xmm4,xmm3
  0000000140EDC523: minss       xmm3,xmm2
  0000000140EDC527: andnps      xmm0,xmm3
  0000000140EDC52A: orps        xmm0,xmm4
  0000000140EDC52D: movlps      qword ptr [rsp+50h],xmm1
  0000000140EDC532: movsd       xmm2,mmword ptr [r12]
  0000000140EDC538: subps       xmm2,xmm1
  0000000140EDC53B: movaps      xmm3,xmm2
  0000000140EDC53E: mulps       xmm3,xmm2
  0000000140EDC541: movshdup    xmm4,xmm3
  0000000140EDC545: addss       xmm4,xmm3
  0000000140EDC549: xorps       xmm3,xmm3
  0000000140EDC54C: sqrtss      xmm3,xmm4
  0000000140EDC550: movaps      xmm4,xmm7
  0000000140EDC553: divss       xmm4,xmm3
  0000000140EDC557: movaps      xmm3,xmm4
  0000000140EDC55A: xorps       xmm3,xmm9
  0000000140EDC55E: unpcklps    xmm3,xmm4
  0000000140EDC561: shufps      xmm2,xmm2,0E1h
  0000000140EDC565: mulps       xmm2,xmm3
  0000000140EDC568: movaps      xmm3,xmm11
  0000000140EDC56C: xorps       xmm3,xmm9
  0000000140EDC570: movlhps     xmm2,xmm3
  0000000140EDC573: movups      xmmword ptr [rsp+40h],xmm2
  0000000140EDC578: mov         qword ptr [rsp+58h],0
  0000000140EDC581: mov         dword ptr [rsp+60h],1
  0000000140EDC589: movss       dword ptr [rsp+64h],xmm0
  0000000140EDC58F: movshdup    xmm2,xmm1
  0000000140EDC593: movss       dword ptr [rsp+28h],xmm0
  0000000140EDC599: mov         dword ptr [rsp+20h],1
  0000000140EDC5A1: mov         rcx,qword ptr [rsp+70h]
  0000000140EDC5A6: mov         r9,r15
  0000000140EDC5A9: call        0000000140EDBB60
  0000000140EDC5AE: ucomiss     xmm13,xmm8
  0000000140EDC5B2: jb          0000000140EDC3E9
  0000000140EDC5B8: ucomiss     xmm7,xmm13
  0000000140EDC5BC: jb          0000000140EDC3E9
  0000000140EDC5C2: movsldup    xmm0,xmm13
  0000000140EDC5C7: mulps       xmm12,xmm0
  0000000140EDC5CB: addps       xmm6,xmm12
  0000000140EDC5CF: test        sil,sil
  0000000140EDC5D2: je          0000000140EDC2AD
  0000000140EDC5D8: mov         rbx,qword ptr [rsp+320h]
  0000000140EDC5E0: mov         rsi,qword ptr [rbx+10h]
  0000000140EDC5E4: cmp         rsi,qword ptr [rbx]
  0000000140EDC5E7: jne         0000000140EDC291
  0000000140EDC5ED: mov         rcx,rbx
  0000000140EDC5F0: lea         rdx,[142B95EF0h]
  0000000140EDC5F7: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDC5FC: jmp         0000000140EDC291
  0000000140EDC601: movsd       xmm9,mmword ptr [r15+4]
  0000000140EDC607: movss       xmm0,dword ptr [r15+0Ch]
  0000000140EDC60D: movsd       mmword ptr [rsp+90h],xmm9
  0000000140EDC617: movss       dword ptr [rsp+98h],xmm0
  0000000140EDC620: mov         dword ptr [rsp+9Ch],0
  0000000140EDC62B: movsd       xmm10,mmword ptr [r14+4]
  0000000140EDC631: movss       xmm0,dword ptr [r14+0Ch]
  0000000140EDC637: movsd       mmword ptr [rsp+0B0h],xmm10
  0000000140EDC641: movss       dword ptr [rsp+0B8h],xmm0
  0000000140EDC64A: mov         dword ptr [rsp+0BCh],0
  0000000140EDC655: lea         rcx,[rsp+40h]
  0000000140EDC65A: lea         rdx,[rsp+90h]
  0000000140EDC662: lea         r8,[rsp+0B0h]
  0000000140EDC66A: call        _ZN5utils8geometry6circle8Circle2d16intersect_circle17h3acc033072b84e26E
  0000000140EDC66F: cmp         dword ptr [rsp+40h],1
  0000000140EDC674: jne         0000000140EDD1D6
  0000000140EDC67A: movsd       xmm7,mmword ptr [rsp+44h]
  0000000140EDC680: movsd       xmm6,mmword ptr [rsp+4Ch]
  0000000140EDC686: movshdup    xmm2,xmm7
  0000000140EDC68A: movshdup    xmm8,xmm6
  0000000140EDC68F: movsd       mmword ptr [rsp+0C0h],xmm7
  0000000140EDC698: movaps      xmm1,xmm10
  0000000140EDC69C: subps       xmm1,xmm7
  0000000140EDC69F: movaps      xmm0,xmm7
  0000000140EDC6A2: subps       xmm0,xmm9
  0000000140EDC6A6: movaps      xmm3,xmm0
  0000000140EDC6A9: mulps       xmm3,xmm0
  0000000140EDC6AC: movshdup    xmm4,xmm3
  0000000140EDC6B0: addss       xmm4,xmm3
  0000000140EDC6B4: xorps       xmm3,xmm3
  0000000140EDC6B7: sqrtss      xmm3,xmm4
  0000000140EDC6BB: movss       xmm12,dword ptr [__real@3f800000]
  0000000140EDC6C4: movaps      xmm4,xmm12
  0000000140EDC6C8: divss       xmm4,xmm3
  0000000140EDC6CC: movaps      xmm11,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDC6D4: movaps      xmm3,xmm4
  0000000140EDC6D7: xorps       xmm3,xmm11
  0000000140EDC6DB: unpcklps    xmm3,xmm4
  0000000140EDC6DE: shufps      xmm0,xmm1,11h
  0000000140EDC6E2: mulps       xmm1,xmm1
  0000000140EDC6E5: movshdup    xmm4,xmm1
  0000000140EDC6E9: addss       xmm4,xmm1
  0000000140EDC6ED: xorps       xmm1,xmm1
  0000000140EDC6F0: sqrtss      xmm1,xmm4
  0000000140EDC6F4: movaps      xmm4,xmm12
  0000000140EDC6F8: divss       xmm4,xmm1
  0000000140EDC6FC: movaps      xmm1,xmm4
  0000000140EDC6FF: xorps       xmm1,xmm11
  0000000140EDC703: unpcklps    xmm1,xmm4
  0000000140EDC706: movlhps     xmm3,xmm1
  0000000140EDC709: mulps       xmm3,xmm0
  0000000140EDC70C: movups      xmmword ptr [rsp+0B0h],xmm3
  0000000140EDC714: mov         qword ptr [rsp+0C8h],0
  0000000140EDC720: mov         dword ptr [rsp+0D0h],0
  0000000140EDC72B: mov         dword ptr [rsp+20h],0
  0000000140EDC733: lea         r9,[rsp+0B0h]
  0000000140EDC73B: mov         rdi,qword ptr [rsp+70h]
  0000000140EDC740: mov         rcx,rdi
  0000000140EDC743: movaps      xmm1,xmm7
  0000000140EDC746: call        0000000140EDBB60
  0000000140EDC74B: movsd       mmword ptr [rsp+0C0h],xmm6
  0000000140EDC754: shufps      xmm9,xmm6,11h
  0000000140EDC759: movaps      xmm0,xmm6
  0000000140EDC75C: shufps      xmm0,xmm10,11h
  0000000140EDC761: subps       xmm9,xmm0
  0000000140EDC765: movaps      xmm0,xmm9
  0000000140EDC769: mulps       xmm0,xmm9
  0000000140EDC76D: movshdup    xmm1,xmm0
  0000000140EDC771: addss       xmm1,xmm0
  0000000140EDC775: sqrtss      xmm1,xmm1
  0000000140EDC779: movaps      xmm2,xmm12
  0000000140EDC77D: divss       xmm2,xmm1
  0000000140EDC781: movaps      xmm1,xmm2
  0000000140EDC784: xorps       xmm1,xmm11
  0000000140EDC788: movaps      xmm3,xmm0
  0000000140EDC78B: unpckhpd    xmm3,xmm0
  0000000140EDC78F: shufps      xmm0,xmm0,0FFh
  0000000140EDC793: addss       xmm0,xmm3
  0000000140EDC797: sqrtss      xmm0,xmm0
  0000000140EDC79B: divss       xmm12,xmm0
  0000000140EDC7A0: xorps       xmm11,xmm12
  0000000140EDC7A4: unpcklps    xmm11,xmm12
  0000000140EDC7A8: unpcklps    xmm1,xmm2
  0000000140EDC7AB: movlhps     xmm1,xmm11
  0000000140EDC7AF: mulps       xmm1,xmm9
  0000000140EDC7B3: movaps      xmmword ptr [rsp+0B0h],xmm1
  0000000140EDC7BB: mov         qword ptr [rsp+0C8h],1
  0000000140EDC7C7: mov         dword ptr [rsp+0D0h],0
  0000000140EDC7D2: mov         dword ptr [rsp+20h],0
  0000000140EDC7DA: lea         r9,[rsp+0B0h]
  0000000140EDC7E2: mov         rcx,rdi
  0000000140EDC7E5: movaps      xmm1,xmm6
  0000000140EDC7E8: movaps      xmm2,xmm8
  0000000140EDC7EC: call        0000000140EDBB60
  0000000140EDC7F1: test        sil,sil
  0000000140EDC7F4: je          0000000140EDD1D6
  0000000140EDC7FA: mov         rdi,qword ptr [rbx+10h]
  0000000140EDC7FE: cmp         rdi,qword ptr [rbx]
  0000000140EDC801: jne         0000000140EDC812
  0000000140EDC803: lea         rdx,[142B95EA8h]
  0000000140EDC80A: mov         rcx,rbx
  0000000140EDC80D: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDC812: mov         rax,qword ptr [rbx+8]
  0000000140EDC816: lea         rcx,[rdi+rdi*2]
  0000000140EDC81A: mov         dword ptr [rax+rcx*4],0
  0000000140EDC821: movlps      qword ptr [rax+rcx*4+4],xmm7
  0000000140EDC826: lea         rsi,[rdi+1]
  0000000140EDC82A: mov         qword ptr [rbx+10h],rsi
  0000000140EDC82E: cmp         rsi,qword ptr [rbx]
  0000000140EDC831: jne         0000000140EDC842
  0000000140EDC833: lea         rdx,[142B95EC0h]
  0000000140EDC83A: mov         rcx,rbx
  0000000140EDC83D: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDC842: mov         rax,qword ptr [rbx+8]
  0000000140EDC846: lea         rcx,[rsi+rsi*2]
  0000000140EDC84A: mov         dword ptr [rax+rcx*4],1
  0000000140EDC851: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDC856: add         rdi,2
  0000000140EDC85A: mov         qword ptr [rbx+10h],rdi
  0000000140EDC85E: jmp         0000000140EDD1D6
  0000000140EDC863: and         ecx,0FFFFFFFEh
  0000000140EDC866: lea         rdx,[rsp+1C8h]
  0000000140EDC86E: xor         r8d,r8d
  0000000140EDC871: nop         word ptr cs:[rax+rax]
  0000000140EDC880: movsd       xmm0,mmword ptr [rdx+r8*8-8]
  0000000140EDC887: movsd       mmword ptr [rsp+r8*8+160h],xmm0
  0000000140EDC891: mov         qword ptr [rsp+180h],0
  0000000140EDC89D: inc         qword ptr [rsp+188h]
  0000000140EDC8A5: movsd       xmm0,mmword ptr [rdx+r8*8]
  0000000140EDC8AB: movsd       mmword ptr [rsp+r8*8+168h],xmm0
  0000000140EDC8B5: mov         qword ptr [rsp+180h],0
  0000000140EDC8C1: inc         qword ptr [rsp+188h]
  0000000140EDC8C9: lea         r9,[r8+2]
  0000000140EDC8CD: mov         r8,r9
  0000000140EDC8D0: cmp         rcx,r9
  0000000140EDC8D3: jne         0000000140EDC880
  0000000140EDC8D5: test        al,1
  0000000140EDC8D7: je          0000000140EDC8FD
  0000000140EDC8D9: movsd       xmm0,mmword ptr [r12+r9*8]
  0000000140EDC8DF: movsd       mmword ptr [rsp+r9*8+160h],xmm0
  0000000140EDC8E9: mov         qword ptr [rsp+180h],0
  0000000140EDC8F5: inc         qword ptr [rsp+188h]
  0000000140EDC8FD: mov         rax,qword ptr [rsp+1B8h]
  0000000140EDC905: mov         qword ptr [rsp+0B0h],4
  0000000140EDC911: mov         qword ptr [rsp+0B8h],rax
  0000000140EDC919: movups      xmm0,xmmword ptr [rsp+160h]
  0000000140EDC921: movups      xmm1,xmmword ptr [rsp+170h]
  0000000140EDC929: mov         rax,qword ptr [rsp+180h]
  0000000140EDC931: mov         rcx,qword ptr [rsp+188h]
  0000000140EDC939: movups      xmmword ptr [rsp+0C0h],xmm0
  0000000140EDC941: movups      xmmword ptr [rsp+0D0h],xmm1
  0000000140EDC949: mov         qword ptr [rsp+0E0h],rax
  0000000140EDC951: mov         qword ptr [rsp+0E8h],rcx
  0000000140EDC959: movups      xmm0,xmmword ptr [rsp+1B8h]
  0000000140EDC961: movups      xmm1,xmmword ptr [rsp+1C8h]
  0000000140EDC969: movups      xmm2,xmmword ptr [rsp+1D8h]
  0000000140EDC971: movups      xmmword ptr [rsp+0F0h],xmm0
  0000000140EDC979: movups      xmmword ptr [rsp+100h],xmm1
  0000000140EDC981: movups      xmmword ptr [rsp+110h],xmm2
  0000000140EDC989: mov         rax,qword ptr [rsp+1E8h]
  0000000140EDC991: mov         qword ptr [rsp+120h],rax
  0000000140EDC999: mov         dword ptr [rsp+128h],0
  0000000140EDC9A4: lea         rcx,[rsp+14Ch]
  0000000140EDC9AC: lea         rdx,[rsp+0B0h]
  0000000140EDC9B4: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h3ed50a73902c7de7E
  0000000140EDC9B9: cmp         dword ptr [rsp+14Ch],1
  0000000140EDC9C1: jne         0000000140EDD1D6
  0000000140EDC9C7: lea         rdi,[rsp+200h]
  0000000140EDC9CF: lea         rbp,[rsp+78h]
  0000000140EDC9D4: lea         r12,[rsp+14Ch]
  0000000140EDC9DC: lea         r13,[rsp+0B0h]
  0000000140EDC9E4: jmp         0000000140EDCA09
  0000000140EDC9E6: nop         word ptr cs:[rax+rax]
  0000000140EDC9F0: mov         rcx,r12
  0000000140EDC9F3: mov         rdx,r13
  0000000140EDC9F6: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h3ed50a73902c7de7E
  0000000140EDC9FB: test        byte ptr [rsp+14Ch],1
  0000000140EDCA03: je          0000000140EDD1D6
  0000000140EDCA09: movsd       xmm15,mmword ptr [rsp+150h]
  0000000140EDCA13: movsd       xmm6,mmword ptr [rsp+158h]
  0000000140EDCA1C: subps       xmm6,xmm15
  0000000140EDCA20: movaps      xmm0,xmm15
  0000000140EDCA24: movlhps     xmm0,xmm6
  0000000140EDCA27: movaps      xmmword ptr [rsp+1F0h],xmm0
  0000000140EDCA2F: mov         rcx,rdi
  0000000140EDCA32: lea         rdx,[rsp+1F0h]
  0000000140EDCA3A: call        _ZN5utils8geometry4aabb5Aabb213intersect_ray17hff7b0ed451cdfc8bE
  0000000140EDCA3F: movss       dword ptr [rsp+78h],xmm0
  0000000140EDCA45: movss       dword ptr [rsp+7Ch],xmm1
  0000000140EDCA4B: mov         rcx,rbp
  0000000140EDCA4E: call        _ZN5utils8geometry4aabb18RayBoxIntersection6is_hit17h0f3d7b05baffa692E
  0000000140EDCA53: test        al,al
  0000000140EDCA55: je          0000000140EDC9F0
  0000000140EDCA57: movss       xmm0,dword ptr [rsp+78h]
  0000000140EDCA5D: movss       xmm14,dword ptr [rsp+7Ch]
  0000000140EDCA64: movaps      xmm1,xmm6
  0000000140EDCA67: unpcklps    xmm1,xmm6
  0000000140EDCA6A: mulps       xmm1,xmmword ptr [rsp+80h]
  0000000140EDCA72: movaps      xmm2,xmm1
  0000000140EDCA75: unpckhpd    xmm2,xmm1
  0000000140EDCA79: addps       xmm2,xmm1
  0000000140EDCA7C: movaps      xmm1,xmm2
  0000000140EDCA7F: mulps       xmm1,xmm2
  0000000140EDCA82: movshdup    xmm3,xmm1
  0000000140EDCA86: addss       xmm3,xmm1
  0000000140EDCA8A: xorps       xmm1,xmm1
  0000000140EDCA8D: sqrtss      xmm1,xmm3
  0000000140EDCA91: movss       xmm3,dword ptr [__real@3f800000]
  0000000140EDCA99: divss       xmm3,xmm1
  0000000140EDCA9D: movsldup    xmm13,xmm3
  0000000140EDCAA2: mulps       xmm13,xmm2
  0000000140EDCAA6: ucomiss     xmm0,dword ptr [__real@00000000]
  0000000140EDCAAD: jb          0000000140EDCE46
  0000000140EDCAB3: movss       xmm1,dword ptr [__real@3f800000]
  0000000140EDCABB: ucomiss     xmm1,xmm0
  0000000140EDCABE: jb          0000000140EDCE46
  0000000140EDCAC4: movsldup    xmm8,xmm0
  0000000140EDCAC9: mulps       xmm8,xmm6
  0000000140EDCACD: addps       xmm8,xmm15
  0000000140EDCAD1: movaps      xmm0,xmm8
  0000000140EDCAD5: unpcklps    xmm0,xmm8
  0000000140EDCAD9: mulps       xmm0,xmmword ptr [rsp+80h]
  0000000140EDCAE1: movaps      xmm7,xmm0
  0000000140EDCAE4: unpckhpd    xmm7,xmm0
  0000000140EDCAE8: addps       xmm7,xmm0
  0000000140EDCAEB: addps       xmm7,xmmword ptr [rsp+1A0h]
  0000000140EDCAF3: test        sil,sil
  0000000140EDCAF6: je          0000000140EDCB30
  0000000140EDCAF8: mov         rsi,qword ptr [rbx+10h]
  0000000140EDCAFC: cmp         rsi,qword ptr [rbx]
  0000000140EDCAFF: jne         0000000140EDCB10
  0000000140EDCB01: mov         rcx,rbx
  0000000140EDCB04: lea         rdx,[142B95F08h]
  0000000140EDCB0B: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDCB10: mov         rax,qword ptr [rbx+8]
  0000000140EDCB14: lea         rcx,[rsi+rsi*2]
  0000000140EDCB18: mov         dword ptr [rax+rcx*4],1
  0000000140EDCB1F: movlps      qword ptr [rax+rcx*4+4],xmm7
  0000000140EDCB24: inc         rsi
  0000000140EDCB27: mov         qword ptr [rbx+10h],rsi
  0000000140EDCB2B: movzx       esi,byte ptr [rsp+3Fh]
  0000000140EDCB30: divps       xmm8,xmmword ptr [rsp+190h]
  0000000140EDCB39: movaps      xmm0,xmm8
  0000000140EDCB3D: andps       xmm0,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  0000000140EDCB44: pshufd      xmm1,xmm0,0F5h
  0000000140EDCB49: ucomiss     xmm0,xmm1
  0000000140EDCB4C: jbe         0000000140EDCB93
  0000000140EDCB4E: movaps      xmm0,xmm8
  0000000140EDCB52: movaps      xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDCB59: xorps       xmm0,xmm1
  0000000140EDCB5C: andps       xmm0,xmm1
  0000000140EDCB5F: orps        xmm0,xmmword ptr [__xmm@3f8000003f8000003f8000003f800000]
  0000000140EDCB66: cmpunordss  xmm8,xmm8
  0000000140EDCB6C: movaps      xmm1,xmm8
  0000000140EDCB70: movss       xmm2,dword ptr [__real@ffc00000]
  0000000140EDCB78: andps       xmm1,xmm2
  0000000140EDCB7B: andnps      xmm8,xmm0
  0000000140EDCB7F: orps        xmm8,xmm1
  0000000140EDCB83: movq        xmm9,xmm8
  0000000140EDCB88: shufps      xmm9,xmmword ptr [__xmm@00000000000000000000000000000000],0E2h
  0000000140EDCB91: jmp         0000000140EDCBCF
  0000000140EDCB93: movshdup    xmm0,xmm8
  0000000140EDCB98: movaps      xmm1,xmm0
  0000000140EDCB9B: andps       xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDCBA2: orps        xmm1,xmmword ptr [__xmm@3f8000003f8000003f8000003f800000]
  0000000140EDCBA9: cmpunordss  xmm0,xmm0
  0000000140EDCBAE: movaps      xmm2,xmm0
  0000000140EDCBB1: movss       xmm3,dword ptr [__real@7fc00000]
  0000000140EDCBB9: andps       xmm2,xmm3
  0000000140EDCBBC: andnps      xmm0,xmm1
  0000000140EDCBBF: orps        xmm0,xmm2
  0000000140EDCBC2: movaps      xmm9,xmmword ptr [__xmm@00000000000000008000000000000000]
  0000000140EDCBCA: movss       xmm9,xmm0
  0000000140EDCBCF: movshdup    xmm8,xmm7
  0000000140EDCBD4: mov         rcx,r15
  0000000140EDCBD7: movaps      xmm1,xmm7
  0000000140EDCBDA: movaps      xmm2,xmm8
  0000000140EDCBDE: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDCBE3: movaps      xmm11,xmm0
  0000000140EDCBE7: mov         rcx,r14
  0000000140EDCBEA: movaps      xmm1,xmm7
  0000000140EDCBED: movaps      xmm2,xmm8
  0000000140EDCBF1: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDCBF6: movaps      xmm10,xmm0
  0000000140EDCBFA: mov         rcx,r15
  0000000140EDCBFD: movaps      xmm1,xmm11
  0000000140EDCC01: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDCC06: movaps      xmm12,xmm0
  0000000140EDCC0A: movaps      xmm11,xmm1
  0000000140EDCC0E: mov         rcx,r14
  0000000140EDCC11: movaps      xmm1,xmm10
  0000000140EDCC15: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDCC1A: mulss       xmm0,xmm12
  0000000140EDCC1F: mulss       xmm1,xmm11
  0000000140EDCC24: addss       xmm1,xmm0
  0000000140EDCC28: movss       xmm0,dword ptr [__real@3f666666]
  0000000140EDCC30: ucomiss     xmm0,xmm1
  0000000140EDCC33: jbe         0000000140EDCE46
  0000000140EDCC39: unpcklps    xmm9,xmm9
  0000000140EDCC3D: mulps       xmm9,xmmword ptr [rsp+80h]
  0000000140EDCC46: movaps      xmm11,xmm9
  0000000140EDCC4A: unpckhpd    xmm11,xmm9
  0000000140EDCC4F: addps       xmm11,xmm9
  0000000140EDCC53: mov         rcx,r15
  0000000140EDCC56: xor         edx,edx
  0000000140EDCC58: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCC5D: unpcklps    xmm0,xmm1
  0000000140EDCC60: movaps      xmm1,xmm7
  0000000140EDCC63: subps       xmm1,xmm0
  0000000140EDCC66: mulps       xmm1,xmm1
  0000000140EDCC69: movshdup    xmm0,xmm1
  0000000140EDCC6D: addss       xmm0,xmm1
  0000000140EDCC71: xorps       xmm9,xmm9
  0000000140EDCC75: sqrtss      xmm9,xmm0
  0000000140EDCC7A: minss       xmm9,dword ptr [__real@42c80000]
  0000000140EDCC83: mov         edx,1
  0000000140EDCC88: mov         rcx,r15
  0000000140EDCC8B: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCC90: unpcklps    xmm0,xmm1
  0000000140EDCC93: movaps      xmm1,xmm7
  0000000140EDCC96: subps       xmm1,xmm0
  0000000140EDCC99: mulps       xmm1,xmm1
  0000000140EDCC9C: movshdup    xmm0,xmm1
  0000000140EDCCA0: addss       xmm0,xmm1
  0000000140EDCCA4: xorps       xmm10,xmm10
  0000000140EDCCA8: sqrtss      xmm10,xmm0
  0000000140EDCCAD: minss       xmm10,xmm9
  0000000140EDCCB2: mov         edx,2
  0000000140EDCCB7: mov         rcx,r15
  0000000140EDCCBA: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCCBF: unpcklps    xmm0,xmm1
  0000000140EDCCC2: movaps      xmm1,xmm7
  0000000140EDCCC5: subps       xmm1,xmm0
  0000000140EDCCC8: mulps       xmm1,xmm1
  0000000140EDCCCB: movshdup    xmm0,xmm1
  0000000140EDCCCF: addss       xmm0,xmm1
  0000000140EDCCD3: xorps       xmm9,xmm9
  0000000140EDCCD7: sqrtss      xmm9,xmm0
  0000000140EDCCDC: minss       xmm9,xmm10
  0000000140EDCCE1: mov         edx,3
  0000000140EDCCE6: mov         rcx,r15
  0000000140EDCCE9: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCCEE: unpcklps    xmm0,xmm1
  0000000140EDCCF1: movaps      xmm1,xmm7
  0000000140EDCCF4: subps       xmm1,xmm0
  0000000140EDCCF7: mulps       xmm1,xmm1
  0000000140EDCCFA: movshdup    xmm0,xmm1
  0000000140EDCCFE: addss       xmm0,xmm1
  0000000140EDCD02: xorps       xmm10,xmm10
  0000000140EDCD06: sqrtss      xmm10,xmm0
  0000000140EDCD0B: minss       xmm10,xmm9
  0000000140EDCD10: mov         rcx,r14
  0000000140EDCD13: xor         edx,edx
  0000000140EDCD15: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCD1A: unpcklps    xmm0,xmm1
  0000000140EDCD1D: movaps      xmm1,xmm7
  0000000140EDCD20: subps       xmm1,xmm0
  0000000140EDCD23: mulps       xmm1,xmm1
  0000000140EDCD26: movshdup    xmm0,xmm1
  0000000140EDCD2A: addss       xmm0,xmm1
  0000000140EDCD2E: xorps       xmm9,xmm9
  0000000140EDCD32: sqrtss      xmm9,xmm0
  0000000140EDCD37: minss       xmm9,xmm10
  0000000140EDCD3C: mov         edx,1
  0000000140EDCD41: mov         rcx,r14
  0000000140EDCD44: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCD49: unpcklps    xmm0,xmm1
  0000000140EDCD4C: movaps      xmm1,xmm7
  0000000140EDCD4F: subps       xmm1,xmm0
  0000000140EDCD52: mulps       xmm1,xmm1
  0000000140EDCD55: movshdup    xmm0,xmm1
  0000000140EDCD59: addss       xmm0,xmm1
  0000000140EDCD5D: xorps       xmm10,xmm10
  0000000140EDCD61: sqrtss      xmm10,xmm0
  0000000140EDCD66: minss       xmm10,xmm9
  0000000140EDCD6B: mov         edx,2
  0000000140EDCD70: mov         rcx,r14
  0000000140EDCD73: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCD78: unpcklps    xmm0,xmm1
  0000000140EDCD7B: movaps      xmm1,xmm7
  0000000140EDCD7E: subps       xmm1,xmm0
  0000000140EDCD81: mulps       xmm1,xmm1
  0000000140EDCD84: movshdup    xmm0,xmm1
  0000000140EDCD88: addss       xmm0,xmm1
  0000000140EDCD8C: sqrtss      xmm0,xmm0
  0000000140EDCD90: movaps      xmm1,xmm0
  0000000140EDCD93: minss       xmm1,xmm10
  0000000140EDCD98: cmpunordss  xmm10,xmm10
  0000000140EDCD9E: movaps      xmm2,xmm10
  0000000140EDCDA2: andnps      xmm2,xmm1
  0000000140EDCDA5: andps       xmm10,xmm0
  0000000140EDCDA9: orps        xmm10,xmm2
  0000000140EDCDAD: mov         edx,3
  0000000140EDCDB2: mov         rcx,r14
  0000000140EDCDB5: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCDBA: unpcklps    xmm0,xmm1
  0000000140EDCDBD: movaps      xmm1,xmm7
  0000000140EDCDC0: subps       xmm1,xmm0
  0000000140EDCDC3: mulps       xmm1,xmm1
  0000000140EDCDC6: movshdup    xmm0,xmm1
  0000000140EDCDCA: addss       xmm0,xmm1
  0000000140EDCDCE: sqrtss      xmm0,xmm0
  0000000140EDCDD2: movaps      xmm1,xmm0
  0000000140EDCDD5: minss       xmm1,xmm10
  0000000140EDCDDA: cmpunordss  xmm10,xmm10
  0000000140EDCDE0: movaps      xmm2,xmm10
  0000000140EDCDE4: andnps      xmm2,xmm1
  0000000140EDCDE7: andps       xmm10,xmm0
  0000000140EDCDEB: orps        xmm10,xmm2
  0000000140EDCDEF: movlps      qword ptr [rsp+50h],xmm7
  0000000140EDCDF4: movaps      xmm0,xmm13
  0000000140EDCDF8: xorps       xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDCDFF: movlhps     xmm11,xmm0
  0000000140EDCE03: movups      xmmword ptr [rsp+40h],xmm11
  0000000140EDCE09: mov         qword ptr [rsp+58h],0
  0000000140EDCE12: mov         dword ptr [rsp+60h],1
  0000000140EDCE1A: movss       dword ptr [rsp+64h],xmm10
  0000000140EDCE21: movss       dword ptr [rsp+28h],xmm10
  0000000140EDCE28: mov         dword ptr [rsp+20h],1
  0000000140EDCE30: mov         rcx,qword ptr [rsp+70h]
  0000000140EDCE35: movaps      xmm1,xmm7
  0000000140EDCE38: movaps      xmm2,xmm8
  0000000140EDCE3C: lea         r9,[rsp+40h]
  0000000140EDCE41: call        0000000140EDBB60
  0000000140EDCE46: ucomiss     xmm14,dword ptr [__real@00000000]
  0000000140EDCE4E: jb          0000000140EDC9F0
  0000000140EDCE54: movss       xmm0,dword ptr [__real@3f800000]
  0000000140EDCE5C: ucomiss     xmm0,xmm14
  0000000140EDCE60: jb          0000000140EDC9F0
  0000000140EDCE66: movsldup    xmm0,xmm14
  0000000140EDCE6B: mulps       xmm6,xmm0
  0000000140EDCE6E: addps       xmm15,xmm6
  0000000140EDCE72: movaps      xmm0,xmm15
  0000000140EDCE76: unpcklps    xmm0,xmm15
  0000000140EDCE7A: mulps       xmm0,xmmword ptr [rsp+80h]
  0000000140EDCE82: movaps      xmm7,xmm0
  0000000140EDCE85: unpckhpd    xmm7,xmm0
  0000000140EDCE89: addps       xmm7,xmm0
  0000000140EDCE8C: addps       xmm7,xmmword ptr [rsp+1A0h]
  0000000140EDCE94: test        sil,sil
  0000000140EDCE97: je          0000000140EDCED1
  0000000140EDCE99: mov         rsi,qword ptr [rbx+10h]
  0000000140EDCE9D: cmp         rsi,qword ptr [rbx]
  0000000140EDCEA0: jne         0000000140EDCEB1
  0000000140EDCEA2: mov         rcx,rbx
  0000000140EDCEA5: lea         rdx,[142B95F20h]
  0000000140EDCEAC: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDCEB1: mov         rax,qword ptr [rbx+8]
  0000000140EDCEB5: lea         rcx,[rsi+rsi*2]
  0000000140EDCEB9: mov         dword ptr [rax+rcx*4],0
  0000000140EDCEC0: movlps      qword ptr [rax+rcx*4+4],xmm7
  0000000140EDCEC5: inc         rsi
  0000000140EDCEC8: mov         qword ptr [rbx+10h],rsi
  0000000140EDCECC: movzx       esi,byte ptr [rsp+3Fh]
  0000000140EDCED1: divps       xmm15,xmmword ptr [rsp+190h]
  0000000140EDCEDA: movaps      xmm0,xmm15
  0000000140EDCEDE: andps       xmm0,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  0000000140EDCEE5: pshufd      xmm1,xmm0,0F5h
  0000000140EDCEEA: ucomiss     xmm0,xmm1
  0000000140EDCEED: jbe         0000000140EDCF2C
  0000000140EDCEEF: movaps      xmm0,xmm15
  0000000140EDCEF3: andps       xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDCEFA: orps        xmm0,xmmword ptr [__xmm@3f8000003f8000003f8000003f800000]
  0000000140EDCF01: cmpunordss  xmm15,xmm15
  0000000140EDCF07: movaps      xmm1,xmm15
  0000000140EDCF0B: movss       xmm2,dword ptr [__real@7fc00000]
  0000000140EDCF13: andps       xmm1,xmm2
  0000000140EDCF16: andnps      xmm15,xmm0
  0000000140EDCF1A: orps        xmm15,xmm1
  0000000140EDCF1E: movaps      xmm12,xmmword ptr [__xmm@00000000000000000000000080000000]
  0000000140EDCF26: unpcklps    xmm12,xmm15
  0000000140EDCF2A: jmp         0000000140EDCF6A
  0000000140EDCF2C: movshdup    xmm0,xmm15
  0000000140EDCF31: movaps      xmm1,xmm0
  0000000140EDCF34: movaps      xmm2,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140EDCF3B: xorps       xmm1,xmm2
  0000000140EDCF3E: andps       xmm1,xmm2
  0000000140EDCF41: orps        xmm1,xmmword ptr [__xmm@3f8000003f8000003f8000003f800000]
  0000000140EDCF48: cmpunordss  xmm0,xmm0
  0000000140EDCF4D: movaps      xmm2,xmm0
  0000000140EDCF50: movss       xmm3,dword ptr [__real@ffc00000]
  0000000140EDCF58: andps       xmm2,xmm3
  0000000140EDCF5B: andnps      xmm0,xmm1
  0000000140EDCF5E: orps        xmm0,xmm2
  0000000140EDCF61: xorps       xmm12,xmm12
  0000000140EDCF65: movss       xmm12,xmm0
  0000000140EDCF6A: movshdup    xmm8,xmm7
  0000000140EDCF6F: mov         rcx,r15
  0000000140EDCF72: movaps      xmm1,xmm7
  0000000140EDCF75: movaps      xmm2,xmm8
  0000000140EDCF79: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDCF7E: movaps      xmm9,xmm0
  0000000140EDCF82: mov         rcx,r14
  0000000140EDCF85: movaps      xmm1,xmm7
  0000000140EDCF88: movaps      xmm2,xmm8
  0000000140EDCF8C: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDCF91: movaps      xmm10,xmm0
  0000000140EDCF95: mov         rcx,r15
  0000000140EDCF98: movaps      xmm1,xmm9
  0000000140EDCF9C: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDCFA1: movaps      xmm9,xmm0
  0000000140EDCFA5: movaps      xmm11,xmm1
  0000000140EDCFA9: mov         rcx,r14
  0000000140EDCFAC: movaps      xmm1,xmm10
  0000000140EDCFB0: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDCFB5: mulss       xmm0,xmm9
  0000000140EDCFBA: mulss       xmm1,xmm11
  0000000140EDCFBF: addss       xmm1,xmm0
  0000000140EDCFC3: movss       xmm0,dword ptr [__real@3f666666]
  0000000140EDCFCB: ucomiss     xmm0,xmm1
  0000000140EDCFCE: jbe         0000000140EDC9F0
  0000000140EDCFD4: unpcklps    xmm12,xmm12
  0000000140EDCFD8: mulps       xmm12,xmmword ptr [rsp+80h]
  0000000140EDCFE1: movaps      xmm6,xmm12
  0000000140EDCFE5: unpckhpd    xmm6,xmm12
  0000000140EDCFEA: addps       xmm6,xmm12
  0000000140EDCFEE: mov         rcx,r15
  0000000140EDCFF1: xor         edx,edx
  0000000140EDCFF3: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDCFF8: unpcklps    xmm0,xmm1
  0000000140EDCFFB: movaps      xmm1,xmm7
  0000000140EDCFFE: subps       xmm1,xmm0
  0000000140EDD001: mulps       xmm1,xmm1
  0000000140EDD004: movshdup    xmm0,xmm1
  0000000140EDD008: addss       xmm0,xmm1
  0000000140EDD00C: xorps       xmm9,xmm9
  0000000140EDD010: sqrtss      xmm9,xmm0
  0000000140EDD015: minss       xmm9,dword ptr [__real@42c80000]
  0000000140EDD01E: mov         edx,1
  0000000140EDD023: mov         rcx,r15
  0000000140EDD026: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD02B: unpcklps    xmm0,xmm1
  0000000140EDD02E: movaps      xmm1,xmm7
  0000000140EDD031: subps       xmm1,xmm0
  0000000140EDD034: mulps       xmm1,xmm1
  0000000140EDD037: movshdup    xmm0,xmm1
  0000000140EDD03B: addss       xmm0,xmm1
  0000000140EDD03F: xorps       xmm10,xmm10
  0000000140EDD043: sqrtss      xmm10,xmm0
  0000000140EDD048: minss       xmm10,xmm9
  0000000140EDD04D: mov         edx,2
  0000000140EDD052: mov         rcx,r15
  0000000140EDD055: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD05A: unpcklps    xmm0,xmm1
  0000000140EDD05D: movaps      xmm1,xmm7
  0000000140EDD060: subps       xmm1,xmm0
  0000000140EDD063: mulps       xmm1,xmm1
  0000000140EDD066: movshdup    xmm0,xmm1
  0000000140EDD06A: addss       xmm0,xmm1
  0000000140EDD06E: xorps       xmm9,xmm9
  0000000140EDD072: sqrtss      xmm9,xmm0
  0000000140EDD077: minss       xmm9,xmm10
  0000000140EDD07C: mov         edx,3
  0000000140EDD081: mov         rcx,r15
  0000000140EDD084: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD089: unpcklps    xmm0,xmm1
  0000000140EDD08C: movaps      xmm1,xmm7
  0000000140EDD08F: subps       xmm1,xmm0
  0000000140EDD092: mulps       xmm1,xmm1
  0000000140EDD095: movshdup    xmm0,xmm1
  0000000140EDD099: addss       xmm0,xmm1
  0000000140EDD09D: xorps       xmm10,xmm10
  0000000140EDD0A1: sqrtss      xmm10,xmm0
  0000000140EDD0A6: minss       xmm10,xmm9
  0000000140EDD0AB: mov         rcx,r14
  0000000140EDD0AE: xor         edx,edx
  0000000140EDD0B0: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD0B5: unpcklps    xmm0,xmm1
  0000000140EDD0B8: movaps      xmm1,xmm7
  0000000140EDD0BB: subps       xmm1,xmm0
  0000000140EDD0BE: mulps       xmm1,xmm1
  0000000140EDD0C1: movshdup    xmm0,xmm1
  0000000140EDD0C5: addss       xmm0,xmm1
  0000000140EDD0C9: sqrtss      xmm11,xmm0
  0000000140EDD0CE: minss       xmm11,xmm10
  0000000140EDD0D3: mov         edx,1
  0000000140EDD0D8: mov         rcx,r14
  0000000140EDD0DB: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD0E0: unpcklps    xmm0,xmm1
  0000000140EDD0E3: movaps      xmm1,xmm7
  0000000140EDD0E6: subps       xmm1,xmm0
  0000000140EDD0E9: mulps       xmm1,xmm1
  0000000140EDD0EC: movshdup    xmm0,xmm1
  0000000140EDD0F0: addss       xmm0,xmm1
  0000000140EDD0F4: xorps       xmm9,xmm9
  0000000140EDD0F8: sqrtss      xmm9,xmm0
  0000000140EDD0FD: minss       xmm9,xmm11
  0000000140EDD102: mov         edx,2
  0000000140EDD107: mov         rcx,r14
  0000000140EDD10A: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD10F: unpcklps    xmm0,xmm1
  0000000140EDD112: movaps      xmm1,xmm7
  0000000140EDD115: subps       xmm1,xmm0
  0000000140EDD118: mulps       xmm1,xmm1
  0000000140EDD11B: movshdup    xmm0,xmm1
  0000000140EDD11F: addss       xmm0,xmm1
  0000000140EDD123: sqrtss      xmm0,xmm0
  0000000140EDD127: movaps      xmm1,xmm0
  0000000140EDD12A: minss       xmm1,xmm9
  0000000140EDD12F: cmpunordss  xmm9,xmm9
  0000000140EDD135: movaps      xmm2,xmm9
  0000000140EDD139: andnps      xmm2,xmm1
  0000000140EDD13C: andps       xmm9,xmm0
  0000000140EDD140: orps        xmm9,xmm2
  0000000140EDD144: mov         edx,3
  0000000140EDD149: mov         rcx,r14
  0000000140EDD14C: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD151: unpcklps    xmm0,xmm1
  0000000140EDD154: movaps      xmm1,xmm7
  0000000140EDD157: subps       xmm1,xmm0
  0000000140EDD15A: mulps       xmm1,xmm1
  0000000140EDD15D: movshdup    xmm0,xmm1
  0000000140EDD161: addss       xmm0,xmm1
  0000000140EDD165: sqrtss      xmm0,xmm0
  0000000140EDD169: movaps      xmm1,xmm0
  0000000140EDD16C: minss       xmm1,xmm9
  0000000140EDD171: cmpunordss  xmm9,xmm9
  0000000140EDD177: movaps      xmm2,xmm9
  0000000140EDD17B: andnps      xmm2,xmm1
  0000000140EDD17E: andps       xmm9,xmm0
  0000000140EDD182: orps        xmm9,xmm2
  0000000140EDD186: movlps      qword ptr [rsp+50h],xmm7
  0000000140EDD18B: movlhps     xmm6,xmm13
  0000000140EDD18F: movups      xmmword ptr [rsp+40h],xmm6
  0000000140EDD194: mov         qword ptr [rsp+58h],1
  0000000140EDD19D: mov         dword ptr [rsp+60h],1
  0000000140EDD1A5: movss       dword ptr [rsp+64h],xmm9
  0000000140EDD1AC: movss       dword ptr [rsp+28h],xmm9
  0000000140EDD1B3: mov         dword ptr [rsp+20h],1
  0000000140EDD1BB: mov         rcx,qword ptr [rsp+70h]
  0000000140EDD1C0: movaps      xmm1,xmm7
  0000000140EDD1C3: movaps      xmm2,xmm8
  0000000140EDD1C7: lea         r9,[rsp+40h]
  0000000140EDD1CC: call        0000000140EDBB60
  0000000140EDD1D1: jmp         0000000140EDC9F0
  0000000140EDD1D6: movaps      xmm6,xmmword ptr [rsp+210h]
  0000000140EDD1DE: movaps      xmm7,xmmword ptr [rsp+220h]
  0000000140EDD1E6: movaps      xmm8,xmmword ptr [rsp+230h]
  0000000140EDD1EF: movaps      xmm9,xmmword ptr [rsp+240h]
  0000000140EDD1F8: movaps      xmm10,xmmword ptr [rsp+250h]
  0000000140EDD201: movaps      xmm11,xmmword ptr [rsp+260h]
  0000000140EDD20A: movaps      xmm12,xmmword ptr [rsp+270h]
  0000000140EDD213: movaps      xmm13,xmmword ptr [rsp+280h]
  0000000140EDD21C: movaps      xmm14,xmmword ptr [rsp+290h]
  0000000140EDD225: movaps      xmm15,xmmword ptr [rsp+2A0h]
  0000000140EDD22E: add         rsp,2B8h
  0000000140EDD235: pop         rbx
  0000000140EDD236: pop         rbp
  0000000140EDD237: pop         rdi
  0000000140EDD238: pop         rsi
  0000000140EDD239: pop         r12
  0000000140EDD23B: pop         r13
  0000000140EDD23D: pop         r14
  0000000140EDD23F: pop         r15
  0000000140EDD241: ret
  0000000140EDD242: CC CC CC CC CC CC CC CC CC CC CC CC CC CC        ..............


_ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners16intersect_shapes17hfb6d4c38c159379cE:
  0000000140EDD250: push        r15
  0000000140EDD252: push        r14
  0000000140EDD254: push        r13
  0000000140EDD256: push        r12
  0000000140EDD258: push        rsi
  0000000140EDD259: push        rdi
  0000000140EDD25A: push        rbp
  0000000140EDD25B: push        rbx
  0000000140EDD25C: sub         rsp,258h
  0000000140EDD263: movaps      xmmword ptr [rsp+240h],xmm15
  0000000140EDD26C: movaps      xmmword ptr [rsp+230h],xmm14
  0000000140EDD275: movaps      xmmword ptr [rsp+220h],xmm13
  0000000140EDD27E: movaps      xmmword ptr [rsp+210h],xmm12
  0000000140EDD287: movaps      xmmword ptr [rsp+200h],xmm11
  0000000140EDD290: movaps      xmmword ptr [rsp+1F0h],xmm10
  0000000140EDD299: movaps      xmmword ptr [rsp+1E0h],xmm9
  0000000140EDD2A2: movaps      xmmword ptr [rsp+1D0h],xmm8
  0000000140EDD2AB: movaps      xmmword ptr [rsp+1C0h],xmm7
  0000000140EDD2B3: movaps      xmmword ptr [rsp+1B0h],xmm6
  0000000140EDD2BB: mov         rsi,r9
  0000000140EDD2BE: mov         ebx,r8d
  0000000140EDD2C1: mov         rdi,rdx
  0000000140EDD2C4: mov         r14,rcx
  0000000140EDD2C7: mov         ecx,dword ptr [rcx]
  0000000140EDD2C9: movzx       eax,byte ptr [rdx]
  0000000140EDD2CC: mov         dword ptr [rsp+3Ch],ecx
  0000000140EDD2D0: test        ecx,ecx
  0000000140EDD2D2: mov         byte ptr [rsp+3Bh],r8b
  0000000140EDD2D7: je          0000000140EDD44D
  0000000140EDD2DD: test        al,1
  0000000140EDD2DF: je          0000000140EDD45A
  0000000140EDD2E5: add         r14,4
  0000000140EDD2E9: add         rdi,4
  0000000140EDD2ED: lea         rcx,[rsp+0C0h]
  0000000140EDD2F5: mov         rdx,r14
  0000000140EDD2F8: call        _ZN5utils8geometry9rectangle11Rectangle2d26as_transformation_and_size17h6846edc8a380f996E
  0000000140EDD2FD: movaps      xmm9,xmmword ptr [rsp+0C0h]
  0000000140EDD306: movsd       xmm10,mmword ptr [rsp+0D0h]
  0000000140EDD310: movss       xmm6,dword ptr [rsp+0E0h]
  0000000140EDD319: movss       xmm11,dword ptr [rsp+0E4h]
  0000000140EDD323: lea         rcx,[rsp+0C0h]
  0000000140EDD32B: mov         rdx,rdi
  0000000140EDD32E: call        _ZN5utils8geometry9rectangle11Rectangle2d26as_transformation_and_size17h6846edc8a380f996E
  0000000140EDD333: movaps      xmm8,xmmword ptr [rsp+0C0h]
  0000000140EDD33C: movsd       xmm7,mmword ptr [rsp+0D0h]
  0000000140EDD345: unpcklps    xmm7,xmm7
  0000000140EDD348: movss       dword ptr [rsp+20h],xmm11
  0000000140EDD34F: lea         rcx,[rsp+1A0h]
  0000000140EDD357: xorps       xmm1,xmm1
  0000000140EDD35A: xorps       xmm2,xmm2
  0000000140EDD35D: movaps      xmm3,xmm6
  0000000140EDD360: call        _ZN5utils8geometry4aabb5Aabb216from_center_dims17h1107e02350cfd9a4E
  0000000140EDD365: movaps      xmm0,xmm9
  0000000140EDD369: shufps      xmm0,xmm9,0EBh
  0000000140EDD36E: mulps       xmm0,xmm9
  0000000140EDD372: movshdup    xmm1,xmm0
  0000000140EDD376: subss       xmm0,xmm1
  0000000140EDD37A: shufps      xmm0,xmm0,0
  0000000140EDD37E: movaps      xmm1,xmmword ptr [__xmm@3f800000bf800000bf8000003f800000]
  0000000140EDD385: divps       xmm1,xmm0
  0000000140EDD388: movaps      xmm0,xmm9
  0000000140EDD38C: shufps      xmm0,xmm9,27h
  0000000140EDD391: mulps       xmm0,xmm1
  0000000140EDD394: movaps      xmm1,xmm10
  0000000140EDD398: unpcklps    xmm1,xmm10
  0000000140EDD39C: mulps       xmm1,xmm0
  0000000140EDD39F: movaps      xmm2,xmm1
  0000000140EDD3A2: unpckhpd    xmm2,xmm1
  0000000140EDD3A6: addps       xmm2,xmm1
  0000000140EDD3A9: movaps      xmm1,xmm8
  0000000140EDD3AD: unpcklps    xmm1,xmm8
  0000000140EDD3B1: unpckhps    xmm8,xmm8
  0000000140EDD3B5: mulps       xmm1,xmm0
  0000000140EDD3B8: mulps       xmm8,xmm0
  0000000140EDD3BC: movaps      xmm3,xmm1
  0000000140EDD3BF: unpckhpd    xmm3,xmm8
  0000000140EDD3C4: movlhps     xmm1,xmm8
  0000000140EDD3C8: addps       xmm1,xmm3
  0000000140EDD3CB: mulps       xmm0,xmm7
  0000000140EDD3CE: movaps      xmm3,xmm0
  0000000140EDD3D1: unpckhpd    xmm3,xmm0
  0000000140EDD3D5: addps       xmm3,xmm0
  0000000140EDD3D8: subps       xmm3,xmm2
  0000000140EDD3DB: movaps      xmmword ptr [rsp+60h],xmm1
  0000000140EDD3E0: movlps      qword ptr [rsp+70h],xmm3
  0000000140EDD3E5: lea         r15,[rsp+160h]
  0000000140EDD3ED: mov         rcx,r15
  0000000140EDD3F0: mov         rdx,rdi
  0000000140EDD3F3: call        _ZN5utils8geometry9rectangle11Rectangle2d27as_points2_centered_aligned17h13f0455834b1169cE
  0000000140EDD3F8: mov         qword ptr [rsp+180h],0
  0000000140EDD404: mov         qword ptr [rsp+188h],4
  0000000140EDD410: lea         rax,[rsp+60h]
  0000000140EDD415: mov         qword ptr [rsp+158h],rax
  0000000140EDD41D: xorps       xmm0,xmm0
  0000000140EDD420: movups      xmmword ptr [rsp+0B0h],xmm0
  0000000140EDD428: xor         eax,eax
  0000000140EDD42A: test        al,al
  0000000140EDD42C: jne         0000000140EDD82D
  0000000140EDD432: mov         eax,4
  0000000140EDD437: mov         ecx,4
  0000000140EDD43C: cmp         ecx,1
  0000000140EDD43F: jne         0000000140EDD79E
  0000000140EDD445: xor         r9d,r9d
  0000000140EDD448: jmp         0000000140EDD805
  0000000140EDD44D: test        al,1
  0000000140EDD44F: je          0000000140EDD6B8
  0000000140EDD455: mov         r15,r14
  0000000140EDD458: jmp         0000000140EDD460
  0000000140EDD45A: mov         r15,rdi
  0000000140EDD45D: mov         rdi,r14
  0000000140EDD460: add         rdi,4
  0000000140EDD464: lea         rcx,[rsp+60h]
  0000000140EDD469: mov         rdx,rdi
  0000000140EDD46C: call        _ZN5utils8geometry9rectangle11Rectangle2d10as_points217h4b26c106db0a5c9aE
  0000000140EDD471: mov         qword ptr [rsp+80h],0
  0000000140EDD47D: mov         qword ptr [rsp+88h],4
  0000000140EDD489: movaps      xmm0,xmmword ptr [rsp+70h]
  0000000140EDD48E: movaps      xmm1,xmmword ptr [rsp+60h]
  0000000140EDD493: movups      xmmword ptr [rsp+0F8h],xmm1
  0000000140EDD49B: movaps      xmm1,xmmword ptr [rsp+60h]
  0000000140EDD4A0: movaps      xmm2,xmmword ptr [rsp+70h]
  0000000140EDD4A5: movups      xmmword ptr [rsp+108h],xmm2
  0000000140EDD4AD: mov         rax,qword ptr [rsp+80h]
  0000000140EDD4B5: mov         qword ptr [rsp+118h],rax
  0000000140EDD4BD: mov         rax,qword ptr [rsp+88h]
  0000000140EDD4C5: mov         qword ptr [rsp+120h],rax
  0000000140EDD4CD: movups      xmmword ptr [rsp+0C8h],xmm1
  0000000140EDD4D5: movups      xmmword ptr [rsp+0D8h],xmm0
  0000000140EDD4DD: mov         qword ptr [rsp+0E8h],0
  0000000140EDD4E9: mov         qword ptr [rsp+0F0h],4
  0000000140EDD4F5: mov         dword ptr [rsp+128h],0
  0000000140EDD500: mov         qword ptr [rsp+0C0h],4
  0000000140EDD50C: lea         rcx,[rsp+60h]
  0000000140EDD511: lea         rdx,[rsp+0C0h]
  0000000140EDD519: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h64c1a140eb3a9994E
  0000000140EDD51E: cmp         dword ptr [rsp+60h],1
  0000000140EDD523: jne         0000000140EDDBFF
  0000000140EDD529: add         r15,4
  0000000140EDD52D: mov         r12d,dword ptr [rsp+3Ch]
  0000000140EDD532: xor         r12d,1
  0000000140EDD536: lea         r14,[rsp+40h]
  0000000140EDD53B: movss       xmm7,dword ptr [__real@3f800000]
  0000000140EDD543: xorps       xmm8,xmm8
  0000000140EDD547: movss       xmm9,dword ptr [__real@3f800000]
  0000000140EDD550: lea         rbp,[rsp+60h]
  0000000140EDD555: lea         r13,[rsp+0C0h]
  0000000140EDD55D: jmp         0000000140EDD5D3
  0000000140EDD55F: movsldup    xmm0,xmm11
  0000000140EDD564: mulps       xmm10,xmm0
  0000000140EDD568: addps       xmm6,xmm10
  0000000140EDD56C: mov         rax,qword ptr [rsi+8]
  0000000140EDD570: lea         rcx,[rbx+rbx*2]
  0000000140EDD574: mov         edx,dword ptr [rsp+3Ch]
  0000000140EDD578: mov         dword ptr [rax+rcx*4],edx
  0000000140EDD57B: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDD580: inc         rbx
  0000000140EDD583: mov         qword ptr [rsi+10h],rbx
  0000000140EDD587: movzx       ebx,byte ptr [rsp+3Bh]
  0000000140EDD58C: mov         rcx,rdi
  0000000140EDD58F: xor         edx,edx
  0000000140EDD591: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD596: mov         edx,1
  0000000140EDD59B: mov         rcx,rdi
  0000000140EDD59E: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD5A3: mov         edx,2
  0000000140EDD5A8: mov         rcx,rdi
  0000000140EDD5AB: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD5B0: mov         edx,3
  0000000140EDD5B5: mov         rcx,rdi
  0000000140EDD5B8: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDD5BD: mov         rcx,rbp
  0000000140EDD5C0: mov         rdx,r13
  0000000140EDD5C3: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h64c1a140eb3a9994E
  0000000140EDD5C8: test        byte ptr [rsp+60h],1
  0000000140EDD5CD: je          0000000140EDDBFF
  0000000140EDD5D3: movsd       xmm6,mmword ptr [rsp+64h]
  0000000140EDD5D9: movsd       xmm10,mmword ptr [rsp+6Ch]
  0000000140EDD5E0: movshdup    xmm3,xmm6
  0000000140EDD5E4: subps       xmm10,xmm6
  0000000140EDD5E8: movshdup    xmm0,xmm10
  0000000140EDD5ED: movss       dword ptr [rsp+20h],xmm10
  0000000140EDD5F4: movss       dword ptr [rsp+28h],xmm0
  0000000140EDD5FA: mov         rcx,r14
  0000000140EDD5FD: mov         rdx,r15
  0000000140EDD600: movaps      xmm2,xmm6
  0000000140EDD603: call        _ZN5utils8geometry6circle8Circle2d13intersect_ray17h7d4e0430dcf54c87E
  0000000140EDD608: test        byte ptr [rsp+40h],1
  0000000140EDD60D: je          0000000140EDD5BD
  0000000140EDD60F: movss       xmm11,dword ptr [rsp+48h]
  0000000140EDD616: movss       xmm12,dword ptr [rsp+44h]
  0000000140EDD61D: movaps      xmm0,xmm12
  0000000140EDD621: cmpleps     xmm0,xmm7
  0000000140EDD625: xorps       xmm1,xmm1
  0000000140EDD628: cmpleps     xmm1,xmm12
  0000000140EDD62D: andps       xmm1,xmm0
  0000000140EDD630: movd        eax,xmm1
  0000000140EDD634: and         al,bl
  0000000140EDD636: cmp         al,1
  0000000140EDD638: jne         0000000140EDD67B
  0000000140EDD63A: mov         rbx,qword ptr [rsi+10h]
  0000000140EDD63E: cmp         rbx,qword ptr [rsi]
  0000000140EDD641: jne         0000000140EDD652
  0000000140EDD643: mov         rcx,rsi
  0000000140EDD646: lea         rdx,[142B95ED8h]
  0000000140EDD64D: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDD652: movsldup    xmm0,xmm12
  0000000140EDD657: mulps       xmm0,xmm10
  0000000140EDD65B: addps       xmm0,xmm6
  0000000140EDD65E: mov         rax,qword ptr [rsi+8]
  0000000140EDD662: lea         rcx,[rbx+rbx*2]
  0000000140EDD666: mov         dword ptr [rax+rcx*4],r12d
  0000000140EDD66A: movlps      qword ptr [rax+rcx*4+4],xmm0
  0000000140EDD66F: inc         rbx
  0000000140EDD672: mov         qword ptr [rsi+10h],rbx
  0000000140EDD676: movzx       ebx,byte ptr [rsp+3Bh]
  0000000140EDD67B: ucomiss     xmm11,xmm8
  0000000140EDD67F: jb          0000000140EDD5BD
  0000000140EDD685: ucomiss     xmm9,xmm11
  0000000140EDD689: jb          0000000140EDD5BD
  0000000140EDD68F: test        bl,bl
  0000000140EDD691: je          0000000140EDD58C
  0000000140EDD697: mov         rbx,qword ptr [rsi+10h]
  0000000140EDD69B: cmp         rbx,qword ptr [rsi]
  0000000140EDD69E: jne         0000000140EDD55F
  0000000140EDD6A4: mov         rcx,rsi
  0000000140EDD6A7: lea         rdx,[142B95EF0h]
  0000000140EDD6AE: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDD6B3: jmp         0000000140EDD55F
  0000000140EDD6B8: movss       xmm0,dword ptr [r14+0Ch]
  0000000140EDD6BE: movsd       xmm1,mmword ptr [r14+4]
  0000000140EDD6C4: movsd       mmword ptr [rsp+40h],xmm1
  0000000140EDD6CA: movss       dword ptr [rsp+48h],xmm0
  0000000140EDD6D0: mov         dword ptr [rsp+4Ch],0
  0000000140EDD6D8: movss       xmm0,dword ptr [rdi+0Ch]
  0000000140EDD6DD: movsd       xmm1,mmword ptr [rdi+4]
  0000000140EDD6E2: movsd       mmword ptr [rsp+60h],xmm1
  0000000140EDD6E8: movss       dword ptr [rsp+68h],xmm0
  0000000140EDD6EE: mov         dword ptr [rsp+6Ch],0
  0000000140EDD6F6: lea         rcx,[rsp+0C0h]
  0000000140EDD6FE: lea         rdx,[rsp+40h]
  0000000140EDD703: lea         r8,[rsp+60h]
  0000000140EDD708: call        _ZN5utils8geometry6circle8Circle2d16intersect_circle17h3acc033072b84e26E
  0000000140EDD70D: cmp         dword ptr [rsp+0C0h],1
  0000000140EDD715: jne         0000000140EDDBFF
  0000000140EDD71B: test        bl,bl
  0000000140EDD71D: je          0000000140EDDBFF
  0000000140EDD723: movsd       xmm7,mmword ptr [rsp+0C4h]
  0000000140EDD72C: movsd       xmm6,mmword ptr [rsp+0CCh]
  0000000140EDD735: mov         rdi,qword ptr [rsi+10h]
  0000000140EDD739: cmp         rdi,qword ptr [rsi]
  0000000140EDD73C: jne         0000000140EDD74D
  0000000140EDD73E: lea         rdx,[142B95EA8h]
  0000000140EDD745: mov         rcx,rsi
  0000000140EDD748: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDD74D: mov         rax,qword ptr [rsi+8]
  0000000140EDD751: lea         rcx,[rdi+rdi*2]
  0000000140EDD755: mov         dword ptr [rax+rcx*4],0
  0000000140EDD75C: movlps      qword ptr [rax+rcx*4+4],xmm7
  0000000140EDD761: lea         rbx,[rdi+1]
  0000000140EDD765: mov         qword ptr [rsi+10h],rbx
  0000000140EDD769: cmp         rbx,qword ptr [rsi]
  0000000140EDD76C: jne         0000000140EDD77D
  0000000140EDD76E: lea         rdx,[142B95EC0h]
  0000000140EDD775: mov         rcx,rsi
  0000000140EDD778: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDD77D: mov         rax,qword ptr [rsi+8]
  0000000140EDD781: lea         rcx,[rbx+rbx*2]
  0000000140EDD785: mov         dword ptr [rax+rcx*4],1
  0000000140EDD78C: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDD791: add         rdi,2
  0000000140EDD795: mov         qword ptr [rsi+10h],rdi
  0000000140EDD799: jmp         0000000140EDDBFF
  0000000140EDD79E: and         ecx,0FFFFFFFEh
  0000000140EDD7A1: lea         rdx,[rsp+168h]
  0000000140EDD7A9: xor         r8d,r8d
  0000000140EDD7AC: nop         dword ptr [rax]
  0000000140EDD7B0: movsd       xmm0,mmword ptr [rdx+r8*8-8]
  0000000140EDD7B7: movsd       mmword ptr [rsp+r8*8+90h],xmm0
  0000000140EDD7C1: mov         qword ptr [rsp+0B0h],0
  0000000140EDD7CD: inc         qword ptr [rsp+0B8h]
  0000000140EDD7D5: movsd       xmm0,mmword ptr [rdx+r8*8]
  0000000140EDD7DB: movsd       mmword ptr [rsp+r8*8+98h],xmm0
  0000000140EDD7E5: mov         qword ptr [rsp+0B0h],0
  0000000140EDD7F1: inc         qword ptr [rsp+0B8h]
  0000000140EDD7F9: lea         r9,[r8+2]
  0000000140EDD7FD: mov         r8,r9
  0000000140EDD800: cmp         rcx,r9
  0000000140EDD803: jne         0000000140EDD7B0
  0000000140EDD805: test        al,1
  0000000140EDD807: je          0000000140EDD82D
  0000000140EDD809: movsd       xmm0,mmword ptr [r15+r9*8]
  0000000140EDD80F: movsd       mmword ptr [rsp+r9*8+90h],xmm0
  0000000140EDD819: mov         qword ptr [rsp+0B0h],0
  0000000140EDD825: inc         qword ptr [rsp+0B8h]
  0000000140EDD82D: mov         rax,qword ptr [rsp+158h]
  0000000140EDD835: mov         qword ptr [rsp+0C0h],4
  0000000140EDD841: mov         qword ptr [rsp+0C8h],rax
  0000000140EDD849: movups      xmm0,xmmword ptr [rsp+90h]
  0000000140EDD851: movups      xmm1,xmmword ptr [rsp+0A0h]
  0000000140EDD859: mov         rax,qword ptr [rsp+0B0h]
  0000000140EDD861: mov         rcx,qword ptr [rsp+0B8h]
  0000000140EDD869: movups      xmmword ptr [rsp+0D0h],xmm0
  0000000140EDD871: movups      xmmword ptr [rsp+0E0h],xmm1
  0000000140EDD879: mov         qword ptr [rsp+0F0h],rax
  0000000140EDD881: mov         qword ptr [rsp+0F8h],rcx
  0000000140EDD889: movups      xmm0,xmmword ptr [rsp+158h]
  0000000140EDD891: movups      xmm1,xmmword ptr [rsp+168h]
  0000000140EDD899: movups      xmm2,xmmword ptr [rsp+178h]
  0000000140EDD8A1: movups      xmmword ptr [rsp+100h],xmm0
  0000000140EDD8A9: movups      xmmword ptr [rsp+110h],xmm1
  0000000140EDD8B1: movups      xmmword ptr [rsp+120h],xmm2
  0000000140EDD8B9: mov         rax,qword ptr [rsp+188h]
  0000000140EDD8C1: mov         qword ptr [rsp+130h],rax
  0000000140EDD8C9: mov         dword ptr [rsp+138h],0
  0000000140EDD8D4: lea         rcx,[rsp+40h]
  0000000140EDD8D9: lea         rdx,[rsp+0C0h]
  0000000140EDD8E1: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h3ed50a73902c7de7E
  0000000140EDD8E6: cmp         dword ptr [rsp+40h],1
  0000000140EDD8EB: jne         0000000140EDDBFF
  0000000140EDD8F1: lea         r12,[rsp+190h]
  0000000140EDD8F9: lea         r13,[rsp+58h]
  0000000140EDD8FE: xorps       xmm11,xmm11
  0000000140EDD902: movss       xmm12,dword ptr [__real@3f800000]
  0000000140EDD90B: lea         rbp,[rsp+40h]
  0000000140EDD910: lea         r15,[rsp+0C0h]
  0000000140EDD918: jmp         0000000140EDD936
  0000000140EDD91A: nop         word ptr [rax+rax]
  0000000140EDD920: mov         rcx,rbp
  0000000140EDD923: mov         rdx,r15
  0000000140EDD926: call        _ZN115_$LT$itertools..tuple_impl..CircularTupleWindows$LT$I$C$T$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4next17h3ed50a73902c7de7E
  0000000140EDD92B: test        byte ptr [rsp+40h],1
  0000000140EDD930: je          0000000140EDDBFF
  0000000140EDD936: movsd       xmm14,mmword ptr [rsp+44h]
  0000000140EDD93D: movsd       xmm15,mmword ptr [rsp+4Ch]
  0000000140EDD944: subps       xmm15,xmm14
  0000000140EDD948: movaps      xmm0,xmm14
  0000000140EDD94C: movlhps     xmm0,xmm15
  0000000140EDD950: movaps      xmmword ptr [rsp+190h],xmm0
  0000000140EDD958: lea         rcx,[rsp+1A0h]
  0000000140EDD960: mov         rdx,r12
  0000000140EDD963: call        _ZN5utils8geometry4aabb5Aabb213intersect_ray17hff7b0ed451cdfc8bE
  0000000140EDD968: movss       dword ptr [rsp+58h],xmm0
  0000000140EDD96E: movss       dword ptr [rsp+5Ch],xmm1
  0000000140EDD974: mov         rcx,r13
  0000000140EDD977: call        _ZN5utils8geometry4aabb18RayBoxIntersection6is_hit17h0f3d7b05baffa692E
  0000000140EDD97C: test        al,al
  0000000140EDD97E: je          0000000140EDD920
  0000000140EDD980: movss       xmm0,dword ptr [rsp+58h]
  0000000140EDD986: movss       xmm13,dword ptr [rsp+5Ch]
  0000000140EDD98D: ucomiss     xmm0,xmm11
  0000000140EDD991: jb          0000000140EDDABF
  0000000140EDD997: ucomiss     xmm12,xmm0
  0000000140EDD99B: jb          0000000140EDDABF
  0000000140EDD9A1: movsldup    xmm0,xmm0
  0000000140EDD9A5: mulps       xmm0,xmm15
  0000000140EDD9A9: addps       xmm0,xmm14
  0000000140EDD9AD: unpcklps    xmm0,xmm0
  0000000140EDD9B0: mulps       xmm0,xmm9
  0000000140EDD9B4: movaps      xmm6,xmm0
  0000000140EDD9B7: unpckhpd    xmm6,xmm0
  0000000140EDD9BB: addps       xmm6,xmm0
  0000000140EDD9BE: addps       xmm6,xmm10
  0000000140EDD9C2: test        bl,bl
  0000000140EDD9C4: je          0000000140EDD9FE
  0000000140EDD9C6: mov         rbx,qword ptr [rsi+10h]
  0000000140EDD9CA: cmp         rbx,qword ptr [rsi]
  0000000140EDD9CD: jne         0000000140EDD9DE
  0000000140EDD9CF: mov         rcx,rsi
  0000000140EDD9D2: lea         rdx,[142B95F08h]
  0000000140EDD9D9: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDD9DE: mov         rax,qword ptr [rsi+8]
  0000000140EDD9E2: lea         rcx,[rbx+rbx*2]
  0000000140EDD9E6: mov         dword ptr [rax+rcx*4],1
  0000000140EDD9ED: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDD9F2: inc         rbx
  0000000140EDD9F5: mov         qword ptr [rsi+10h],rbx
  0000000140EDD9F9: movzx       ebx,byte ptr [rsp+3Bh]
  0000000140EDD9FE: movshdup    xmm7,xmm6
  0000000140EDDA02: mov         rcx,r14
  0000000140EDDA05: movaps      xmm1,xmm6
  0000000140EDDA08: movaps      xmm2,xmm7
  0000000140EDDA0B: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDDA10: movaps      xmm8,xmm0
  0000000140EDDA14: mov         rcx,rdi
  0000000140EDDA17: movaps      xmm1,xmm6
  0000000140EDDA1A: movaps      xmm2,xmm7
  0000000140EDDA1D: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDDA22: movaps      xmm6,xmm0
  0000000140EDDA25: mov         rcx,r14
  0000000140EDDA28: movaps      xmm1,xmm8
  0000000140EDDA2C: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDDA31: movaps      xmm7,xmm0
  0000000140EDDA34: movaps      xmm8,xmm1
  0000000140EDDA38: mov         rcx,rdi
  0000000140EDDA3B: movaps      xmm1,xmm6
  0000000140EDDA3E: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDDA43: mulss       xmm0,xmm7
  0000000140EDDA47: mulss       xmm1,xmm8
  0000000140EDDA4C: addss       xmm1,xmm0
  0000000140EDDA50: movss       xmm0,dword ptr [__real@3f666666]
  0000000140EDDA58: ucomiss     xmm0,xmm1
  0000000140EDDA5B: jbe         0000000140EDDABF
  0000000140EDDA5D: mov         rcx,r14
  0000000140EDDA60: xor         edx,edx
  0000000140EDDA62: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDA67: mov         edx,1
  0000000140EDDA6C: mov         rcx,r14
  0000000140EDDA6F: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDA74: mov         edx,2
  0000000140EDDA79: mov         rcx,r14
  0000000140EDDA7C: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDA81: mov         edx,3
  0000000140EDDA86: mov         rcx,r14
  0000000140EDDA89: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDA8E: mov         rcx,rdi
  0000000140EDDA91: xor         edx,edx
  0000000140EDDA93: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDA98: mov         edx,1
  0000000140EDDA9D: mov         rcx,rdi
  0000000140EDDAA0: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDAA5: mov         edx,2
  0000000140EDDAAA: mov         rcx,rdi
  0000000140EDDAAD: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDAB2: mov         edx,3
  0000000140EDDAB7: mov         rcx,rdi
  0000000140EDDABA: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDABF: ucomiss     xmm13,xmm11
  0000000140EDDAC3: jb          0000000140EDD920
  0000000140EDDAC9: ucomiss     xmm12,xmm13
  0000000140EDDACD: jb          0000000140EDD920
  0000000140EDDAD3: movsldup    xmm0,xmm13
  0000000140EDDAD8: mulps       xmm15,xmm0
  0000000140EDDADC: addps       xmm14,xmm15
  0000000140EDDAE0: unpcklps    xmm14,xmm14
  0000000140EDDAE4: mulps       xmm14,xmm9
  0000000140EDDAE8: movaps      xmm6,xmm14
  0000000140EDDAEC: unpckhpd    xmm6,xmm14
  0000000140EDDAF1: addps       xmm6,xmm14
  0000000140EDDAF5: addps       xmm6,xmm10
  0000000140EDDAF9: test        bl,bl
  0000000140EDDAFB: je          0000000140EDDB35
  0000000140EDDAFD: mov         rbx,qword ptr [rsi+10h]
  0000000140EDDB01: cmp         rbx,qword ptr [rsi]
  0000000140EDDB04: jne         0000000140EDDB15
  0000000140EDDB06: mov         rcx,rsi
  0000000140EDDB09: lea         rdx,[142B95F20h]
  0000000140EDDB10: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h036c152a69fb4fa5E
  0000000140EDDB15: mov         rax,qword ptr [rsi+8]
  0000000140EDDB19: lea         rcx,[rbx+rbx*2]
  0000000140EDDB1D: mov         dword ptr [rax+rcx*4],0
  0000000140EDDB24: movlps      qword ptr [rax+rcx*4+4],xmm6
  0000000140EDDB29: inc         rbx
  0000000140EDDB2C: mov         qword ptr [rsi+10h],rbx
  0000000140EDDB30: movzx       ebx,byte ptr [rsp+3Bh]
  0000000140EDDB35: movshdup    xmm7,xmm6
  0000000140EDDB39: mov         rcx,r14
  0000000140EDDB3C: movaps      xmm1,xmm6
  0000000140EDDB3F: movaps      xmm2,xmm7
  0000000140EDDB42: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDDB47: movaps      xmm8,xmm0
  0000000140EDDB4B: mov         rcx,rdi
  0000000140EDDB4E: movaps      xmm1,xmm6
  0000000140EDDB51: movaps      xmm2,xmm7
  0000000140EDDB54: call        _ZN5utils8geometry9rectangle11Rectangle2d22get_closest_u_from_pos17h33caa72cd034c32dE
  0000000140EDDB59: movaps      xmm6,xmm0
  0000000140EDDB5C: mov         rcx,r14
  0000000140EDDB5F: movaps      xmm1,xmm8
  0000000140EDDB63: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDDB68: movaps      xmm7,xmm0
  0000000140EDDB6B: movaps      xmm8,xmm1
  0000000140EDDB6F: mov         rcx,rdi
  0000000140EDDB72: movaps      xmm1,xmm6
  0000000140EDDB75: call        _ZN5utils8geometry9rectangle11Rectangle2d15get_normal_at_u17h5fcf5b3bbd8c9a7aE
  0000000140EDDB7A: mulss       xmm0,xmm7
  0000000140EDDB7E: mulss       xmm1,xmm8
  0000000140EDDB83: addss       xmm1,xmm0
  0000000140EDDB87: movss       xmm0,dword ptr [__real@3f666666]
  0000000140EDDB8F: ucomiss     xmm0,xmm1
  0000000140EDDB92: jbe         0000000140EDD920
  0000000140EDDB98: mov         rcx,r14
  0000000140EDDB9B: xor         edx,edx
  0000000140EDDB9D: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBA2: mov         edx,1
  0000000140EDDBA7: mov         rcx,r14
  0000000140EDDBAA: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBAF: mov         edx,2
  0000000140EDDBB4: mov         rcx,r14
  0000000140EDDBB7: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBBC: mov         edx,3
  0000000140EDDBC1: mov         rcx,r14
  0000000140EDDBC4: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBC9: mov         rcx,rdi
  0000000140EDDBCC: xor         edx,edx
  0000000140EDDBCE: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBD3: mov         edx,1
  0000000140EDDBD8: mov         rcx,rdi
  0000000140EDDBDB: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBE0: mov         edx,2
  0000000140EDDBE5: mov         rcx,rdi
  0000000140EDDBE8: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBED: mov         edx,3
  0000000140EDDBF2: mov         rcx,rdi
  0000000140EDDBF5: call        _ZN5utils8geometry9rectangle11Rectangle2d9get_point17he1297970230920a8E
  0000000140EDDBFA: jmp         0000000140EDD920
  0000000140EDDBFF: movaps      xmm6,xmmword ptr [rsp+1B0h]
  0000000140EDDC07: movaps      xmm7,xmmword ptr [rsp+1C0h]
  0000000140EDDC0F: movaps      xmm8,xmmword ptr [rsp+1D0h]
  0000000140EDDC18: movaps      xmm9,xmmword ptr [rsp+1E0h]
  0000000140EDDC21: movaps      xmm10,xmmword ptr [rsp+1F0h]
  0000000140EDDC2A: movaps      xmm11,xmmword ptr [rsp+200h]
  0000000140EDDC33: movaps      xmm12,xmmword ptr [rsp+210h]
  0000000140EDDC3C: movaps      xmm13,xmmword ptr [rsp+220h]
  0000000140EDDC45: movaps      xmm14,xmmword ptr [rsp+230h]
  0000000140EDDC4E: movaps      xmm15,xmmword ptr [rsp+240h]
  0000000140EDDC57: add         rsp,258h
  0000000140EDDC5E: pop         rbx
  0000000140EDDC5F: pop         rbp
  0000000140EDDC60: pop         rdi
  0000000140EDDC61: pop         rsi
  0000000140EDDC62: pop         r12
  0000000140EDDC64: pop         r13
  0000000140EDDC66: pop         r14
  0000000140EDDC68: pop         r15
  0000000140EDDC6A: ret
  0000000140EDDC6B: CC CC CC CC CC                                   .....


_ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners18WallAndRoofChanges11dirty_walls17hbf956cbea14a5ee7E:
  0000000141CD16B0: push        rbp
  0000000141CD16B1: push        r15
  0000000141CD16B3: push        r14
  0000000141CD16B5: push        r13
  0000000141CD16B7: push        r12
  0000000141CD16B9: push        rsi
  0000000141CD16BA: push        rdi
  0000000141CD16BB: push        rbx
  0000000141CD16BC: sub         rsp,1F8h
  0000000141CD16C3: lea         rbp,[rsp+80h]
  0000000141CD16CB: mov         qword ptr [rbp+170h],0FFFFFFFFFFFFFFFEh
  0000000141CD16D6: mov         r11,qword ptr [rdx]
  0000000141CD16D9: mov         r8,qword ptr [rdx+8]
  0000000141CD16DD: mov         r9,qword ptr [r11]
  0000000141CD16E0: xor         eax,eax
  0000000141CD16E2: mov         r10,r9
  0000000141CD16E5: sub         r10,qword ptr [r8+18h]
  0000000141CD16E9: cmovb       r10,rax
  0000000141CD16ED: sub         r9,qword ptr [r8+38h]
  0000000141CD16F1: mov         qword ptr [rbp+138h],rcx
  0000000141CD16F8: cmovb       r9,rax
  0000000141CD16FC: mov         rsi,r10
  0000000141CD16FF: shl         rsi,4
  0000000141CD1703: add         rsi,qword ptr [r8+8]
  0000000141CD1707: mov         rbx,qword ptr [r8+10h]
  0000000141CD170B: mov         rdi,qword ptr [r8+30h]
  0000000141CD170F: sub         rbx,r10
  0000000141CD1712: cmovb       rbx,rax
  0000000141CD1716: mov         ecx,8
  0000000141CD171B: cmovb       rsi,rcx
  0000000141CD171F: mov         r10,r9
  0000000141CD1722: shl         r10,4
  0000000141CD1726: add         r10,qword ptr [r8+28h]
  0000000141CD172A: sub         rdi,r9
  0000000141CD172D: cmovb       rdi,rax
  0000000141CD1731: cmovb       r10,rcx
  0000000141CD1735: lea         r9,[rdi+rbx]
  0000000141CD1739: mov         r8,qword ptr [r8+40h]
  0000000141CD173D: mov         qword ptr [rbp+100h],r9
  0000000141CD1744: sub         r8,r9
  0000000141CD1747: mov         qword ptr [rbp+0D8h],r11
  0000000141CD174E: mov         qword ptr [r11],r8
  0000000141CD1751: shl         rbx,4
  0000000141CD1755: mov         qword ptr [rbp+0E0h],rsi
  0000000141CD175C: add         rbx,rsi
  0000000141CD175F: mov         qword ptr [rbp+0F0h],rbx
  0000000141CD1766: shl         rdi,4
  0000000141CD176A: mov         qword ptr [rbp+0F8h],r10
  0000000141CD1771: add         rdi,r10
  0000000141CD1774: mov         qword ptr [rbp+0E8h],rdi
  0000000141CD177B: mov         r11,qword ptr [rdx+28h]
  0000000141CD177F: mov         r8,qword ptr [rdx+30h]
  0000000141CD1783: mov         r9,qword ptr [r11]
  0000000141CD1786: mov         r10,r9
  0000000141CD1789: sub         r10,qword ptr [r8+18h]
  0000000141CD178D: cmovb       r10,rax
  0000000141CD1791: sub         r9,qword ptr [r8+38h]
  0000000141CD1795: cmovb       r9,rax
  0000000141CD1799: mov         rdi,r10
  0000000141CD179C: shl         rdi,4
  0000000141CD17A0: add         rdi,qword ptr [r8+8]
  0000000141CD17A4: mov         rsi,qword ptr [r8+10h]
  0000000141CD17A8: sub         rsi,r10
  0000000141CD17AB: cmovb       rsi,rax
  0000000141CD17AF: cmovb       rdi,rcx
  0000000141CD17B3: mov         r10,r9
  0000000141CD17B6: shl         r10,4
  0000000141CD17BA: add         r10,qword ptr [r8+28h]
  0000000141CD17BE: mov         r13,qword ptr [r8+30h]
  0000000141CD17C2: sub         r13,r9
  0000000141CD17C5: cmovb       r13,rax
  0000000141CD17C9: cmovb       r10,rcx
  0000000141CD17CD: mov         r8,qword ptr [r8+40h]
  0000000141CD17D1: lea         r9,[rsi+r13]
  0000000141CD17D5: mov         qword ptr [rbp+120h],r9
  0000000141CD17DC: sub         r8,r9
  0000000141CD17DF: mov         qword ptr [rbp+108h],r11
  0000000141CD17E6: mov         qword ptr [r11],r8
  0000000141CD17E9: shl         rsi,4
  0000000141CD17ED: mov         qword ptr [rbp+110h],rdi
  0000000141CD17F4: add         rsi,rdi
  0000000141CD17F7: shl         r13,4
  0000000141CD17FB: mov         qword ptr [rbp+118h],r10
  0000000141CD1802: add         r13,r10
  0000000141CD1805: mov         r10,qword ptr [rdx+50h]
  0000000141CD1809: mov         rdx,qword ptr [rdx+58h]
  0000000141CD180D: mov         r8,qword ptr [r10]
  0000000141CD1810: mov         r9,r8
  0000000141CD1813: sub         r9,qword ptr [rdx+18h]
  0000000141CD1817: cmovb       r9,rax
  0000000141CD181B: sub         r8,qword ptr [rdx+38h]
  0000000141CD181F: cmovb       r8,rax
  0000000141CD1823: mov         r15,r9
  0000000141CD1826: shl         r15,5
  0000000141CD182A: add         r15,qword ptr [rdx+8]
  0000000141CD182E: mov         r12,qword ptr [rdx+10h]
  0000000141CD1832: sub         r12,r9
  0000000141CD1835: cmovb       r12,rax
  0000000141CD1839: cmovb       r15,rcx
  0000000141CD183D: mov         rbx,r8
  0000000141CD1840: shl         rbx,5
  0000000141CD1844: add         rbx,qword ptr [rdx+28h]
  0000000141CD1848: mov         r14,qword ptr [rdx+30h]
  0000000141CD184C: sub         r14,r8
  0000000141CD184F: cmovb       r14,rax
  0000000141CD1853: cmovb       rbx,rcx
  0000000141CD1857: mov         rax,qword ptr [rdx+40h]
  0000000141CD185B: lea         rcx,[r14+r12]
  0000000141CD185F: mov         qword ptr [rbp+130h],rcx
  0000000141CD1866: sub         rax,rcx
  0000000141CD1869: mov         qword ptr [rbp+128h],r10
  0000000141CD1870: mov         qword ptr [r10],rax
  0000000141CD1873: shl         r12,5
  0000000141CD1877: add         r12,r15
  0000000141CD187A: shl         r14,5
  0000000141CD187E: add         r14,rbx
  0000000141CD1881: call        _ZN3std4hash6random11RandomState3new4KEYS29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17he31e4d4088ff0746E
  0000000141CD1886: mov         rdi,rax
  0000000141CD1889: test        byte ptr [rax],1
  0000000141CD188C: je          0000000141CD189F
  0000000141CD188E: mov         rcx,rdi
  0000000141CD1891: add         rcx,8
  0000000141CD1895: mov         rax,qword ptr [rdi+8]
  0000000141CD1899: mov         rdx,qword ptr [rdi+10h]
  0000000141CD189D: jmp         0000000141CD18BA
  0000000141CD189F: call        _ZN3std3sys6random19hashmap_random_keys17h6ad24d26f7fc1598E
  0000000141CD18A4: mov         rcx,rdi
  0000000141CD18A7: add         rcx,8
  0000000141CD18AB: mov         qword ptr [rdi],1
  0000000141CD18B2: mov         qword ptr [rdi+8],rax
  0000000141CD18B6: mov         qword ptr [rdi+10h],rdx
  0000000141CD18BA: lea         r8,[rax+1]
  0000000141CD18BE: mov         qword ptr [rcx],r8
  0000000141CD18C1: movups      xmm0,xmmword ptr [142F33320h]
  0000000141CD18C8: movaps      xmmword ptr [rbp+150h],xmm0
  0000000141CD18CF: movups      xmm0,xmmword ptr [anon.2fc37c2e165e4335d861dbcc334b2b56.1.llvm.2532611877472227773]
  0000000141CD18D6: movaps      xmmword ptr [rbp+140h],xmm0
  0000000141CD18DD: mov         qword ptr [rbp+160h],rax
  0000000141CD18E4: mov         qword ptr [rbp+168h],rdx
  0000000141CD18EB: mov         qword ptr [rbp+40h],1
  0000000141CD18F3: mov         rax,qword ptr [rbp+0D8h]
  0000000141CD18FA: mov         qword ptr [rbp+48h],rax
  0000000141CD18FE: mov         rax,qword ptr [rbp+0E0h]
  0000000141CD1905: mov         qword ptr [rbp+50h],rax
  0000000141CD1909: mov         rax,qword ptr [rbp+0F0h]
  0000000141CD1910: mov         qword ptr [rbp+58h],rax
  0000000141CD1914: mov         rax,qword ptr [rbp+0F8h]
  0000000141CD191B: mov         qword ptr [rbp+60h],rax
  0000000141CD191F: mov         rax,qword ptr [rbp+0E8h]
  0000000141CD1926: mov         qword ptr [rbp+68h],rax
  0000000141CD192A: mov         rax,qword ptr [rbp+100h]
  0000000141CD1931: mov         qword ptr [rbp+70h],rax
  0000000141CD1935: mov         rax,qword ptr [rbp+108h]
  0000000141CD193C: mov         qword ptr [rbp+78h],rax
  0000000141CD1940: mov         rax,qword ptr [rbp+110h]
  0000000141CD1947: mov         qword ptr [rbp+80h],rax
  0000000141CD194E: mov         qword ptr [rbp+88h],rsi
  0000000141CD1955: mov         rax,qword ptr [rbp+118h]
  0000000141CD195C: mov         qword ptr [rbp+90h],rax
  0000000141CD1963: mov         qword ptr [rbp+98h],r13
  0000000141CD196A: mov         rax,qword ptr [rbp+120h]
  0000000141CD1971: mov         qword ptr [rbp+0A0h],rax
  0000000141CD1978: mov         rax,qword ptr [rbp+128h]
  0000000141CD197F: mov         qword ptr [rbp+0A8h],rax
  0000000141CD1986: mov         qword ptr [rbp+0B0h],r15
  0000000141CD198D: mov         qword ptr [rbp+0B8h],r12
  0000000141CD1994: mov         qword ptr [rbp+0C0h],rbx
  0000000141CD199B: mov         qword ptr [rbp+0C8h],r14
  0000000141CD19A2: mov         rax,qword ptr [rbp+130h]
  0000000141CD19A9: mov         qword ptr [rbp+0D0h],rax
  0000000141CD19B0: lea         rcx,[rbp-58h]
  0000000141CD19B4: lea         rdx,[rbp+40h]
  0000000141CD19B8: call        _ZN102_$LT$core..iter..adapters..map..Map$LT$I$C$F$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$9size_hint17h878bc11d913c4a14E.llvm.785564849487419765
  0000000141CD19BD: mov         rdx,qword ptr [rbp-58h]
  0000000141CD19C1: cmp         qword ptr [rbp+150h],rdx
  0000000141CD19C8: jb          0000000141CD1A2A
  0000000141CD19CA: lea         rdi,[rbp-58h]
  0000000141CD19CE: lea         rdx,[rbp+40h]
  0000000141CD19D2: mov         r8d,98h
  0000000141CD19D8: mov         rcx,rdi
  0000000141CD19DB: call        memcpy
  0000000141CD19E0: lea         rdx,[rbp+140h]
  0000000141CD19E7: mov         rcx,rdi
  0000000141CD19EA: call        _ZN106_$LT$core..iter..adapters..chain..Chain$LT$A$C$B$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4fold17h27c3af5177ae99e1E
  0000000141CD19EF: movaps      xmm0,xmmword ptr [rbp+140h]
  0000000141CD19F6: movaps      xmm1,xmmword ptr [rbp+150h]
  0000000141CD19FD: movaps      xmm2,xmmword ptr [rbp+160h]
  0000000141CD1A04: mov         rax,qword ptr [rbp+138h]
  0000000141CD1A0B: movups      xmmword ptr [rax+20h],xmm2
  0000000141CD1A0F: movups      xmmword ptr [rax+10h],xmm1
  0000000141CD1A13: movups      xmmword ptr [rax],xmm0
  0000000141CD1A16: add         rsp,1F8h
  0000000141CD1A1D: pop         rbx
  0000000141CD1A1E: pop         rdi
  0000000141CD1A1F: pop         rsi
  0000000141CD1A20: pop         r12
  0000000141CD1A22: pop         r13
  0000000141CD1A24: pop         r14
  0000000141CD1A26: pop         r15
  0000000141CD1A28: pop         rbp
  0000000141CD1A29: ret
  0000000141CD1A2A: lea         r8,[rbp+160h]
  0000000141CD1A31: lea         rcx,[rbp+140h]
  0000000141CD1A38: mov         r9b,1
  0000000141CD1A3B: call        _ZN9hashbrown3raw21RawTable$LT$T$C$A$GT$14reserve_rehash17h0c815e7171477dfaE
  0000000141CD1A40: jmp         0000000141CD19CA
  0000000141CD1A42: nop         word ptr cs:[rax+rax]
  0000000141CD1A50: mov         qword ptr [rsp+10h],rdx
  0000000141CD1A55: push        rbp
  0000000141CD1A56: push        r15
  0000000141CD1A58: push        r14
  0000000141CD1A5A: push        r13
  0000000141CD1A5C: push        r12
  0000000141CD1A5E: push        rsi
  0000000141CD1A5F: push        rdi
  0000000141CD1A60: push        rbx
  0000000141CD1A61: sub         rsp,28h
  0000000141CD1A65: lea         rbp,[rdx+80h]
  0000000141CD1A6C: lea         rcx,[rbp+140h]
  0000000141CD1A73: call        _ZN4core3ptr100drop_in_place$LT$country_core..resources..stairs..stairs_removed_supports..StairsRemovedSupports$GT$17h6ac75686d43d1941E
  0000000141CD1A78: nop
  0000000141CD1A79: add         rsp,28h
  0000000141CD1A7D: pop         rbx
  0000000141CD1A7E: pop         rdi
  0000000141CD1A7F: pop         rsi
  0000000141CD1A80: pop         r12
  0000000141CD1A82: pop         r13
  0000000141CD1A84: pop         r14
  0000000141CD1A86: pop         r15
  0000000141CD1A88: pop         rbp
  0000000141CD1A89: ret
  0000000141CD1A8A: CC CC CC CC CC CC                                ......


_ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners26detect_intra_shape_corners17ha1d654424b52589cE:
  0000000141CD1A90: push        rbp
  0000000141CD1A91: push        r15
  0000000141CD1A93: push        r14
  0000000141CD1A95: push        r13
  0000000141CD1A97: push        r12
  0000000141CD1A99: push        rsi
  0000000141CD1A9A: push        rdi
  0000000141CD1A9B: push        rbx
  0000000141CD1A9C: sub         rsp,458h
  0000000141CD1AA3: lea         rbp,[rsp+80h]
  0000000141CD1AAB: movaps      xmmword ptr [rbp+3C0h],xmm15
  0000000141CD1AB3: movdqa      xmmword ptr [rbp+3B0h],xmm14
  0000000141CD1ABC: movaps      xmmword ptr [rbp+3A0h],xmm13
  0000000141CD1AC4: movdqa      xmmword ptr [rbp+390h],xmm12
  0000000141CD1ACD: movaps      xmmword ptr [rbp+380h],xmm11
  0000000141CD1AD5: movaps      xmmword ptr [rbp+370h],xmm10
  0000000141CD1ADD: movaps      xmmword ptr [rbp+360h],xmm9
  0000000141CD1AE5: movaps      xmmword ptr [rbp+350h],xmm8
  0000000141CD1AED: movaps      xmmword ptr [rbp+340h],xmm7
  0000000141CD1AF4: movaps      xmmword ptr [rbp+330h],xmm6
  0000000141CD1AFB: mov         qword ptr [rbp+328h],0FFFFFFFFFFFFFFFEh
  0000000141CD1B06: mov         rbx,r9
  0000000141CD1B09: mov         qword ptr [rbp+128h],r8
  0000000141CD1B10: mov         rdi,rdx
  0000000141CD1B13: mov         r13,rcx
  0000000141CD1B16: call        _ZN6puffin13are_scopes_on17h8f511bd2e57f501aE
  0000000141CD1B1B: test        al,al
  0000000141CD1B1D: mov         qword ptr [rbp+2D0h],rdi
  0000000141CD1B24: mov         qword ptr [rbp+2E0h],r13
  0000000141CD1B2B: je          0000000141CD1B8A
  0000000141CD1B2D: mov         r14d,5Ch
  0000000141CD1B33: mov         ecx,5Ch
  0000000141CD1B38: call        0000000141CD1110
  0000000141CD1B3D: lea         r15,[142F3342Bh]
  0000000141CD1B44: cmp         rax,1
  0000000141CD1B48: jne         0000000141CD1BF6
  0000000141CD1B4E: mov         r9,rdx
  0000000141CD1B51: test        rdx,rdx
  0000000141CD1B54: je          0000000141CD1B93
  0000000141CD1B56: cmp         r9,5Ch
  0000000141CD1B5A: jae         0000000141CD1B91
  0000000141CD1B5C: lea         rax,[142F3342Bh]
  0000000141CD1B63: cmp         byte ptr [r9+rax],0BFh
  0000000141CD1B68: jg          0000000141CD1B93
  0000000141CD1B6A: lea         rax,[142F33510h]
  0000000141CD1B71: mov         qword ptr [rsp+20h],rax
  0000000141CD1B76: lea         rcx,[142F3342Bh]
  0000000141CD1B7D: mov         edx,5Ch
  0000000141CD1B82: xor         r8d,r8d
  0000000141CD1B85: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CD1B8A: xor         eax,eax
  0000000141CD1B8C: jmp         0000000141CD1D51
  0000000141CD1B91: jne         0000000141CD1B6A
  0000000141CD1B93: mov         rcx,r9
  0000000141CD1B96: call        0000000141CD1110
  0000000141CD1B9B: cmp         rax,1
  0000000141CD1B9F: jne         0000000141CD1BF6
  0000000141CD1BA1: mov         r8,rdx
  0000000141CD1BA4: add         r8,2
  0000000141CD1BA8: je          0000000141CD1BE3
  0000000141CD1BAA: cmp         r8,5Ch
  0000000141CD1BAE: jae         0000000141CD1BE1
  0000000141CD1BB0: lea         rax,[142F3342Bh]
  0000000141CD1BB7: cmp         byte ptr [r8+rax],0BFh
  0000000141CD1BBC: jg          0000000141CD1BE3
  0000000141CD1BBE: lea         rax,[142F33528h]
  0000000141CD1BC5: mov         qword ptr [rsp+20h],rax
  0000000141CD1BCA: lea         rcx,[142F3342Bh]
  0000000141CD1BD1: mov         edx,5Ch
  0000000141CD1BD6: mov         r9d,5Ch
  0000000141CD1BDC: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CD1BE1: jne         0000000141CD1BBE
  0000000141CD1BE3: mov         r14d,5Ah
  0000000141CD1BE9: sub         r14,rdx
  0000000141CD1BEC: lea         r15,[142F3342Bh]
  0000000141CD1BF3: add         r15,r8
  0000000141CD1BF6: lea         r8,[142F3366Dh]
  0000000141CD1BFD: lea         r12,[142F33620h]
  0000000141CD1C04: nop         word ptr cs:[rax+rax]
  0000000141CD1C10: movsx       eax,byte ptr [r8-1]
  0000000141CD1C15: test        eax,eax
  0000000141CD1C17: js          0000000141CD1C30
  0000000141CD1C19: dec         r8
  0000000141CD1C1C: cmp         eax,5Ch
  0000000141CD1C1F: jne         0000000141CD1C84
  0000000141CD1C21: jmp         0000000141CD1C95
  0000000141CD1C23: nop         word ptr cs:[rax+rax]
  0000000141CD1C30: movzx       ecx,byte ptr [r8-2]
  0000000141CD1C35: cmp         cl,0C0h
  0000000141CD1C38: jge         0000000141CD1C5D
  0000000141CD1C3A: movzx       edx,byte ptr [r8-3]
  0000000141CD1C3F: cmp         dl,0C0h
  0000000141CD1C42: jge         0000000141CD1C66
  0000000141CD1C44: movzx       r9d,byte ptr [r8-4]
  0000000141CD1C49: add         r8,0FFFFFFFFFFFFFFFCh
  0000000141CD1C4D: and         r9d,7
  0000000141CD1C51: shl         r9d,6
  0000000141CD1C55: and         edx,3Fh
  0000000141CD1C58: or          edx,r9d
  0000000141CD1C5B: jmp         0000000141CD1C6D
  0000000141CD1C5D: add         r8,0FFFFFFFFFFFFFFFEh
  0000000141CD1C61: and         ecx,1Fh
  0000000141CD1C64: jmp         0000000141CD1C75
  0000000141CD1C66: add         r8,0FFFFFFFFFFFFFFFDh
  0000000141CD1C6A: and         edx,0Fh
  0000000141CD1C6D: shl         edx,6
  0000000141CD1C70: and         ecx,3Fh
  0000000141CD1C73: or          ecx,edx
  0000000141CD1C75: shl         ecx,6
  0000000141CD1C78: and         al,3Fh
  0000000141CD1C7A: movzx       eax,al
  0000000141CD1C7D: or          eax,ecx
  0000000141CD1C7F: cmp         eax,5Ch
  0000000141CD1C82: je          0000000141CD1C95
  0000000141CD1C84: cmp         eax,2Fh
  0000000141CD1C87: je          0000000141CD1C95
  0000000141CD1C89: cmp         r8,r12
  0000000141CD1C8C: jne         0000000141CD1C10
  0000000141CD1C8E: mov         esi,4Dh
  0000000141CD1C93: jmp         0000000141CD1CE1
  0000000141CD1C95: lea         r12,[142F33620h]
  0000000141CD1C9C: sub         r8,r12
  0000000141CD1C9F: inc         r8
  0000000141CD1CA2: je          0000000141CD1CD6
  0000000141CD1CA4: cmp         r8,4Dh
  0000000141CD1CA8: jae         0000000141CD1CD4
  0000000141CD1CAA: cmp         byte ptr [r8+r12],0BFh
  0000000141CD1CAF: jg          0000000141CD1CD6
  0000000141CD1CB1: lea         rax,[142F334F0h]
  0000000141CD1CB8: mov         qword ptr [rsp+20h],rax
  0000000141CD1CBD: lea         rcx,[142F33620h]
  0000000141CD1CC4: mov         edx,4Dh
  0000000141CD1CC9: mov         r9d,4Dh
  0000000141CD1CCF: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CD1CD4: jne         0000000141CD1CB1
  0000000141CD1CD6: mov         esi,4Dh
  0000000141CD1CDB: sub         rsi,r8
  0000000141CD1CDE: add         r12,r8
  0000000141CD1CE1: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CD1CE6: mov         rcx,rax
  0000000141CD1CE9: mov         rax,qword ptr [rax]
  0000000141CD1CEC: cmp         rax,1
  0000000141CD1CF0: jne         0000000141CD2342
  0000000141CD1CF6: add         rcx,8
  0000000141CD1CFA: cmp         qword ptr [rcx],0
  0000000141CD1CFE: jne         0000000141CD2514
  0000000141CD1D04: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CD1D0B: mov         qword ptr [rbp+2B8h],rcx
  0000000141CD1D12: add         rcx,8
  0000000141CD1D16: mov         qword ptr [rsp+20h],rsi
  0000000141CD1D1B: mov         qword ptr [rsp+30h],0
  0000000141CD1D24: mov         qword ptr [rsp+28h],1
  0000000141CD1D2D: mov         rdx,r15
  0000000141CD1D30: mov         r8,r14
  0000000141CD1D33: mov         r9,r12
  0000000141CD1D36: call        _ZN6puffin14ThreadProfiler11begin_scope17h161a40482b7cfe19E
  0000000141CD1D3B: mov         rcx,qword ptr [rbp+2B8h]
  0000000141CD1D42: inc         qword ptr [rcx]
  0000000141CD1D45: mov         qword ptr [rbp+218h],rax
  0000000141CD1D4C: mov         eax,1
  0000000141CD1D51: mov         qword ptr [rbp+210h],rax
  0000000141CD1D58: mov         r9,qword ptr [rbx]
  0000000141CD1D5B: mov         rdx,qword ptr [rbx+8]
  0000000141CD1D5F: mov         r8,qword ptr [r9]
  0000000141CD1D62: xor         eax,eax
  0000000141CD1D64: mov         rcx,r8
  0000000141CD1D67: sub         rcx,qword ptr [rdx+18h]
  0000000141CD1D6B: cmovb       rcx,rax
  0000000141CD1D6F: sub         r8,qword ptr [rdx+38h]
  0000000141CD1D73: cmovb       r8,rax
  0000000141CD1D77: mov         r10,rcx
  0000000141CD1D7A: shl         r10,4
  0000000141CD1D7E: add         r10,qword ptr [rdx+8]
  0000000141CD1D82: mov         rsi,qword ptr [rdx+10h]
  0000000141CD1D86: mov         r11,qword ptr [rdx+30h]
  0000000141CD1D8A: sub         rsi,rcx
  0000000141CD1D8D: cmovb       rsi,rax
  0000000141CD1D91: mov         ecx,8
  0000000141CD1D96: cmovb       r10,rcx
  0000000141CD1D9A: mov         qword ptr [rbp+250h],r10
  0000000141CD1DA1: mov         r10,r8
  0000000141CD1DA4: shl         r10,4
  0000000141CD1DA8: add         r10,qword ptr [rdx+28h]
  0000000141CD1DAC: sub         r11,r8
  0000000141CD1DAF: cmovb       r11,rax
  0000000141CD1DB3: cmovb       r10,rcx
  0000000141CD1DB7: mov         qword ptr [rbp+310h],r10
  0000000141CD1DBE: mov         qword ptr [rbp+2E8h],r11
  0000000141CD1DC5: mov         qword ptr [rbp+320h],rsi
  0000000141CD1DCC: lea         r8,[r11+rsi]
  0000000141CD1DD0: mov         rdx,qword ptr [rdx+40h]
  0000000141CD1DD4: mov         qword ptr [rbp+300h],r8
  0000000141CD1DDB: sub         rdx,r8
  0000000141CD1DDE: mov         qword ptr [rbp+2B8h],r9
  0000000141CD1DE5: mov         qword ptr [r9],rdx
  0000000141CD1DE8: mov         r10,qword ptr [rbx+28h]
  0000000141CD1DEC: mov         rdx,qword ptr [rbx+30h]
  0000000141CD1DF0: mov         r8,qword ptr [r10]
  0000000141CD1DF3: mov         r9,r8
  0000000141CD1DF6: sub         r9,qword ptr [rdx+18h]
  0000000141CD1DFA: cmovb       r9,rax
  0000000141CD1DFE: sub         r8,qword ptr [rdx+38h]
  0000000141CD1E02: cmovb       r8,rax
  0000000141CD1E06: mov         r11,r9
  0000000141CD1E09: shl         r11,4
  0000000141CD1E0D: add         r11,qword ptr [rdx+8]
  0000000141CD1E11: mov         rdi,qword ptr [rdx+10h]
  0000000141CD1E15: sub         rdi,r9
  0000000141CD1E18: cmovb       rdi,rax
  0000000141CD1E1C: cmovb       r11,rcx
  0000000141CD1E20: mov         qword ptr [rbp+2F0h],r11
  0000000141CD1E27: mov         r9,r8
  0000000141CD1E2A: shl         r9,4
  0000000141CD1E2E: add         r9,qword ptr [rdx+28h]
  0000000141CD1E32: mov         r13,qword ptr [rdx+30h]
  0000000141CD1E36: sub         r13,r8
  0000000141CD1E39: cmovb       r13,rax
  0000000141CD1E3D: cmovb       r9,rcx
  0000000141CD1E41: mov         qword ptr [rbp+2C0h],r9
  0000000141CD1E48: mov         rdx,qword ptr [rdx+40h]
  0000000141CD1E4C: lea         r8,[rdi+r13]
  0000000141CD1E50: mov         qword ptr [rbp+258h],r8
  0000000141CD1E57: sub         rdx,r8
  0000000141CD1E5A: mov         qword ptr [rbp+2D8h],r10
  0000000141CD1E61: mov         qword ptr [r10],rdx
  0000000141CD1E64: mov         r10,qword ptr [rbx+50h]
  0000000141CD1E68: mov         rdx,qword ptr [rbx+58h]
  0000000141CD1E6C: mov         r8,qword ptr [r10]
  0000000141CD1E6F: mov         r9,r8
  0000000141CD1E72: sub         r9,qword ptr [rdx+18h]
  0000000141CD1E76: cmovb       r9,rax
  0000000141CD1E7A: sub         r8,qword ptr [rdx+38h]
  0000000141CD1E7E: cmovb       r8,rax
  0000000141CD1E82: mov         rsi,r9
  0000000141CD1E85: shl         rsi,5
  0000000141CD1E89: add         rsi,qword ptr [rdx+8]
  0000000141CD1E8D: mov         r15,qword ptr [rdx+10h]
  0000000141CD1E91: sub         r15,r9
  0000000141CD1E94: cmovb       r15,rax
  0000000141CD1E98: cmovb       rsi,rcx
  0000000141CD1E9C: mov         r14,r8
  0000000141CD1E9F: shl         r14,5
  0000000141CD1EA3: add         r14,qword ptr [rdx+28h]
  0000000141CD1EA7: mov         r12,qword ptr [rdx+30h]
  0000000141CD1EAB: sub         r12,r8
  0000000141CD1EAE: cmovb       r12,rax
  0000000141CD1EB2: cmovb       r14,rcx
  0000000141CD1EB6: mov         rax,qword ptr [rdx+40h]
  0000000141CD1EBA: lea         rcx,[r12+r15]
  0000000141CD1EBE: mov         qword ptr [rbp+298h],rcx
  0000000141CD1EC5: sub         rax,rcx
  0000000141CD1EC8: mov         qword ptr [rbp+260h],r10
  0000000141CD1ECF: mov         qword ptr [r10],rax
  0000000141CD1ED2: call        _ZN3std4hash6random11RandomState3new4KEYS29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17he31e4d4088ff0746E
  0000000141CD1ED7: mov         rbx,rax
  0000000141CD1EDA: test        byte ptr [rax],1
  0000000141CD1EDD: je          0000000141CD1EF0
  0000000141CD1EDF: mov         rcx,rbx
  0000000141CD1EE2: add         rcx,8
  0000000141CD1EE6: mov         rax,qword ptr [rbx+8]
  0000000141CD1EEA: mov         rdx,qword ptr [rbx+10h]
  0000000141CD1EEE: jmp         0000000141CD1F0B
  0000000141CD1EF0: call        _ZN3std3sys6random19hashmap_random_keys17h6ad24d26f7fc1598E
  0000000141CD1EF5: mov         rcx,rbx
  0000000141CD1EF8: add         rcx,8
  0000000141CD1EFC: mov         qword ptr [rbx],1
  0000000141CD1F03: mov         qword ptr [rbx+8],rax
  0000000141CD1F07: mov         qword ptr [rbx+10h],rdx
  0000000141CD1F0B: lea         r8,[rax+1]
  0000000141CD1F0F: mov         qword ptr [rcx],r8
  0000000141CD1F12: movups      xmm0,xmmword ptr [142F33320h]
  0000000141CD1F19: movaps      xmmword ptr [rbp+230h],xmm0
  0000000141CD1F20: movdqu      xmm0,xmmword ptr [anon.2fc37c2e165e4335d861dbcc334b2b56.1.llvm.2532611877472227773]
  0000000141CD1F28: movdqa      xmmword ptr [rbp+220h],xmm0
  0000000141CD1F30: mov         qword ptr [rbp+240h],rax
  0000000141CD1F37: mov         qword ptr [rbp+248h],rdx
  0000000141CD1F3E: mov         qword ptr [rbp+80h],1
  0000000141CD1F49: mov         rax,qword ptr [rbp+2B8h]
  0000000141CD1F50: mov         qword ptr [rbp+88h],rax
  0000000141CD1F57: mov         rcx,qword ptr [rbp+320h]
  0000000141CD1F5E: shl         rcx,4
  0000000141CD1F62: mov         rax,qword ptr [rbp+250h]
  0000000141CD1F69: add         rcx,rax
  0000000141CD1F6C: mov         qword ptr [rbp+90h],rax
  0000000141CD1F73: mov         qword ptr [rbp+98h],rcx
  0000000141CD1F7A: mov         rax,qword ptr [rbp+2E8h]
  0000000141CD1F81: shl         rax,4
  0000000141CD1F85: mov         rcx,qword ptr [rbp+310h]
  0000000141CD1F8C: add         rax,rcx
  0000000141CD1F8F: mov         qword ptr [rbp+0A0h],rcx
  0000000141CD1F96: mov         qword ptr [rbp+0A8h],rax
  0000000141CD1F9D: mov         rax,qword ptr [rbp+300h]
  0000000141CD1FA4: mov         qword ptr [rbp+0B0h],rax
  0000000141CD1FAB: mov         rax,qword ptr [rbp+2D8h]
  0000000141CD1FB2: mov         qword ptr [rbp+0B8h],rax
  0000000141CD1FB9: shl         rdi,4
  0000000141CD1FBD: mov         rax,qword ptr [rbp+2F0h]
  0000000141CD1FC4: add         rdi,rax
  0000000141CD1FC7: mov         qword ptr [rbp+0C0h],rax
  0000000141CD1FCE: mov         qword ptr [rbp+0C8h],rdi
  0000000141CD1FD5: shl         r13,4
  0000000141CD1FD9: mov         rax,qword ptr [rbp+2C0h]
  0000000141CD1FE0: add         r13,rax
  0000000141CD1FE3: mov         qword ptr [rbp+0D0h],rax
  0000000141CD1FEA: mov         qword ptr [rbp+0D8h],r13
  0000000141CD1FF1: mov         rax,qword ptr [rbp+258h]
  0000000141CD1FF8: mov         qword ptr [rbp+0E0h],rax
  0000000141CD1FFF: mov         rax,qword ptr [rbp+260h]
  0000000141CD2006: mov         qword ptr [rbp+0E8h],rax
  0000000141CD200D: shl         r15,5
  0000000141CD2011: add         r15,rsi
  0000000141CD2014: mov         qword ptr [rbp+0F0h],rsi
  0000000141CD201B: mov         qword ptr [rbp+0F8h],r15
  0000000141CD2022: shl         r12,5
  0000000141CD2026: add         r12,r14
  0000000141CD2029: mov         qword ptr [rbp+100h],r14
  0000000141CD2030: mov         qword ptr [rbp+108h],r12
  0000000141CD2037: mov         rax,qword ptr [rbp+298h]
  0000000141CD203E: mov         qword ptr [rbp+110h],rax
  0000000141CD2045: lea         rcx,[rbp+160h]
  0000000141CD204C: lea         rdx,[rbp+80h]
  0000000141CD2053: call        _ZN102_$LT$core..iter..adapters..map..Map$LT$I$C$F$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$9size_hint17h878bc11d913c4a14E.llvm.785564849487419765
  0000000141CD2058: mov         rdx,qword ptr [rbp+160h]
  0000000141CD205F: cmp         qword ptr [rbp+230h],rdx
  0000000141CD2066: jb          0000000141CD2363
  0000000141CD206C: lea         rbx,[rbp+160h]
  0000000141CD2073: lea         rdx,[rbp+80h]
  0000000141CD207A: mov         r8d,98h
  0000000141CD2080: mov         rcx,rbx
  0000000141CD2083: call        memcpy
  0000000141CD2088: lea         rdx,[rbp+220h]
  0000000141CD208F: mov         rcx,rbx
  0000000141CD2092: call        _ZN106_$LT$core..iter..adapters..chain..Chain$LT$A$C$B$GT$$u20$as$u20$core..iter..traits..iterator..Iterator$GT$4fold17h27c3af5177ae99e1E
  0000000141CD2097: mov         rax,qword ptr [rbp+458h]
  0000000141CD209E: movdqa      xmm0,xmmword ptr [rbp+220h]
  0000000141CD20A6: movdqa      xmm1,xmmword ptr [rbp+230h]
  0000000141CD20AE: movdqa      xmm2,xmmword ptr [rbp+240h]
  0000000141CD20B6: movdqa      xmmword ptr [rbp+80h],xmm0
  0000000141CD20BE: movdqa      xmmword ptr [rbp+90h],xmm1
  0000000141CD20C6: movdqa      xmmword ptr [rbp+0A0h],xmm2
  0000000141CD20CE: mov         ecx,dword ptr [rax+1Ch]
  0000000141CD20D1: mov         rdx,qword ptr [rax+10h]
  0000000141CD20D5: mov         dword ptr [rdx],ecx
  0000000141CD20D7: mov         r12,qword ptr [rax]
  0000000141CD20DA: lea         rdx,[rbp+80h]
  0000000141CD20E1: mov         rcx,r12
  0000000141CD20E4: call        _ZN5alloc3vec16Vec$LT$T$C$A$GT$6retain17hc70147038323c4b1E
  0000000141CD20E9: mov         rax,qword ptr [rbp+460h]
  0000000141CD20F0: mov         ecx,dword ptr [rax+1Ch]
  0000000141CD20F3: mov         rdx,qword ptr [rax+10h]
  0000000141CD20F7: mov         dword ptr [rdx],ecx
  0000000141CD20F9: mov         r13,qword ptr [rax]
  0000000141CD20FC: lea         rdx,[rbp+80h]
  0000000141CD2103: mov         rcx,r13
  0000000141CD2106: call        _ZN9hashbrown3map28HashMap$LT$K$C$V$C$S$C$A$GT$6retain17h1b1b785d3944e544E
  0000000141CD210B: mov         r15,qword ptr [rbp+450h]
  0000000141CD2112: lea         rdx,[rbp+80h]
  0000000141CD2119: mov         rcx,r15
  0000000141CD211C: call        _ZN5alloc3vec16Vec$LT$T$C$A$GT$6retain17h1e2eaa4c25277f55E
  0000000141CD2121: mov         rcx,qword ptr [rbp+440h]
  0000000141CD2128: mov         rax,qword ptr [rcx]
  0000000141CD212B: mov         rcx,qword ptr [rcx+8]
  0000000141CD212F: mov         rdx,qword ptr [rax]
  0000000141CD2132: xor         r11d,r11d
  0000000141CD2135: mov         r8,rdx
  0000000141CD2138: sub         r8,qword ptr [rcx+18h]
  0000000141CD213C: cmovb       r8,r11
  0000000141CD2140: sub         rdx,qword ptr [rcx+38h]
  0000000141CD2144: cmovb       rdx,r11
  0000000141CD2148: mov         r9,qword ptr [rcx+30h]
  0000000141CD214C: sub         r9,rdx
  0000000141CD214F: cmovb       r9,r11
  0000000141CD2153: setbe       dl
  0000000141CD2156: mov         r10,qword ptr [rcx+10h]
  0000000141CD215A: sub         r10,r8
  0000000141CD215D: cmovb       r10,r11
  0000000141CD2161: mov         rcx,qword ptr [rcx+40h]
  0000000141CD2165: setbe       r8b
  0000000141CD2169: add         r10,r9
  0000000141CD216C: mov         r9,rcx
  0000000141CD216F: sub         r9,r10
  0000000141CD2172: mov         qword ptr [rax],r9
  0000000141CD2175: test        r8b,dl
  0000000141CD2178: jne         0000000141CD227B
  0000000141CD217E: mov         qword ptr [rax],rcx
  0000000141CD2181: mov         qword ptr [r12+10h],0
  0000000141CD218A: mov         rsi,qword ptr [r13+18h]
  0000000141CD218E: test        rsi,rsi
  0000000141CD2191: je          0000000141CD2273
  0000000141CD2197: mov         r14,qword ptr [r13]
  0000000141CD219B: movdqa      xmm0,xmmword ptr [r14]
  0000000141CD21A0: pmovmskb    r15d,xmm0
  0000000141CD21A5: not         r15d
  0000000141CD21A8: lea         rdi,[r14+10h]
  0000000141CD21AC: mov         rbx,r14
  0000000141CD21AF: jmp         0000000141CD21CF
  0000000141CD21B1: nop         word ptr cs:[rax+rax]
  0000000141CD21C0: lea         eax,[r15-1]
  0000000141CD21C4: and         eax,r15d
  0000000141CD21C7: mov         r15d,eax
  0000000141CD21CA: dec         rsi
  0000000141CD21CD: je          0000000141CD222F
  0000000141CD21CF: test        r15w,r15w
  0000000141CD21D3: jne         0000000141CD2200
  0000000141CD21D5: nop         word ptr cs:[rax+rax]
  0000000141CD21E0: movdqa      xmm0,xmmword ptr [rdi]
  0000000141CD21E4: pmovmskb    r15d,xmm0
  0000000141CD21E9: add         rbx,0FFFFFFFFFFFFFD80h
  0000000141CD21F0: add         rdi,10h
  0000000141CD21F4: cmp         r15d,0FFFFh
  0000000141CD21FB: je          0000000141CD21E0
  0000000141CD21FD: not         r15d
  0000000141CD2200: tzcnt       eax,r15d
  0000000141CD2205: neg         rax
  0000000141CD2208: lea         rax,[rax+rax*4]
  0000000141CD220C: mov         rdx,qword ptr [rbx+rax*8-18h]
  0000000141CD2211: test        rdx,rdx
  0000000141CD2214: je          0000000141CD21C0
  0000000141CD2216: lea         rax,[rbx+rax*8]
  0000000141CD221A: mov         rcx,qword ptr [rax-10h]
  0000000141CD221E: shl         rdx,3
  0000000141CD2222: mov         r8d,4
  0000000141CD2228: call        __rust_dealloc
  0000000141CD222D: jmp         0000000141CD21C0
  0000000141CD222F: mov         rsi,qword ptr [r13+8]
  0000000141CD2233: test        rsi,rsi
  0000000141CD2236: je          0000000141CD2246
  0000000141CD2238: lea         r8,[rsi+11h]
  0000000141CD223C: mov         rcx,r14
  0000000141CD223F: mov         dl,0FFh
  0000000141CD2241: call        memset
  0000000141CD2246: mov         qword ptr [r13+18h],0
  0000000141CD224E: lea         rax,[rsi+1]
  0000000141CD2252: mov         rcx,rax
  0000000141CD2255: shr         rcx,3
  0000000141CD2259: and         rax,0FFFFFFFFFFFFFFF8h
  0000000141CD225D: sub         rax,rcx
  0000000141CD2260: cmp         rsi,8
  0000000141CD2264: cmovb       rax,rsi
  0000000141CD2268: mov         qword ptr [r13+10h],rax
  0000000141CD226C: mov         r15,qword ptr [rbp+450h]
  0000000141CD2273: mov         qword ptr [r15+10h],0
  0000000141CD227B: mov         qword ptr [rbp+258h],r12
  0000000141CD2282: mov         qword ptr [rbp+260h],r13
  0000000141CD2289: mov         qword ptr [rbp+268h],0
  0000000141CD2294: mov         qword ptr [rbp+270h],4
  0000000141CD229F: mov         qword ptr [rbp+278h],0
  0000000141CD22AA: mov         rbx,qword ptr [rbp+80h]
  0000000141CD22B1: mov         rax,qword ptr [rbp+98h]
  0000000141CD22B8: movdqa      xmm0,xmmword ptr [rbx]
  0000000141CD22BC: pmovmskb    r14d,xmm0
  0000000141CD22C1: not         r14d
  0000000141CD22C4: lea         r13,[rbx+10h]
  0000000141CD22C8: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD22CF: mov         rcx,qword ptr [rcx]
  0000000141CD22D2: mov         qword ptr [rbp+2E0h],rcx
  0000000141CD22D9: lea         rcx,[rcx+20h]
  0000000141CD22DD: mov         qword ptr [rbp+250h],rcx
  0000000141CD22E4: mov         rdx,qword ptr [rbp+2D0h]
  0000000141CD22EB: mov         rcx,qword ptr [rdx]
  0000000141CD22EE: mov         qword ptr [rbp+2F0h],rcx
  0000000141CD22F5: mov         rcx,qword ptr [rdx+8]
  0000000141CD22F9: mov         qword ptr [rbp+300h],rcx
  0000000141CD2300: pcmpeqd     xmm14,xmm14
  0000000141CD2305: movss       xmm7,dword ptr [__real@beeb851f]
  0000000141CD230D: movss       xmm6,dword ptr [__real@be570a3d]
  0000000141CD2315: movss       xmm15,dword ptr [__real@3c23d70a]
  0000000141CD231E: movd        xmm12,dword ptr [__real@3e8f5c29]
  0000000141CD2327: movss       xmm13,dword ptr [__real@3d4ccccd]
  0000000141CD2330: mov         qword ptr [rbp+2D0h],rax
  0000000141CD2337: test        rax,rax
  0000000141CD233A: jne         0000000141CD25BA
  0000000141CD2340: jmp         0000000141CD237E
  0000000141CD2342: test        rax,rax
  0000000141CD2345: jne         0000000141CD2508
  0000000141CD234B: xor         edx,edx
  0000000141CD234D: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CD2352: mov         rcx,rax
  0000000141CD2355: test        rax,rax
  0000000141CD2358: jne         0000000141CD1CFA
  0000000141CD235E: jmp         0000000141CD2508
  0000000141CD2363: lea         r8,[rbp+240h]
  0000000141CD236A: lea         rcx,[rbp+220h]
  0000000141CD2371: mov         r9b,1
  0000000141CD2374: call        _ZN9hashbrown3raw21RawTable$LT$T$C$A$GT$14reserve_rehash17h0c815e7171477dfaE
  0000000141CD2379: jmp         0000000141CD206C
  0000000141CD237E: mov         rbx,qword ptr [r15+8]
  0000000141CD2382: mov         rax,qword ptr [r15+10h]
  0000000141CD2386: lea         rax,[rax+rax*4]
  0000000141CD238A: lea         r14,[rbx+rax*8]
  0000000141CD238E: mov         rax,qword ptr [rbp+448h]
  0000000141CD2395: mov         r15d,dword ptr [rax+1Ch]
  0000000141CD2399: mov         rsi,qword ptr [rax]
  0000000141CD239C: mov         r12,qword ptr [rax+10h]
  0000000141CD23A0: lea         rdi,[rbp+160h]
  0000000141CD23A7: cmp         rbx,r14
  0000000141CD23AA: je          0000000141CD23DD
  0000000141CD23AC: nop         dword ptr [rax]
  0000000141CD23B0: mov         dword ptr [r12],r15d
  0000000141CD23B4: mov         rdx,qword ptr [rbx]
  0000000141CD23B7: movdqu      xmm0,xmmword ptr [rbx+10h]
  0000000141CD23BC: movdqa      xmmword ptr [rbp+160h],xmm0
  0000000141CD23C4: movzx       r9d,byte ptr [rbx+20h]
  0000000141CD23C9: mov         rcx,rsi
  0000000141CD23CC: mov         r8,rdi
  0000000141CD23CF: call        _ZN12country_core9resources5walls10wall_holes9WallHoles3add17h405bcfecc6cfda40E
  0000000141CD23D4: add         rbx,28h
  0000000141CD23D8: cmp         rbx,r14
  0000000141CD23DB: jne         0000000141CD23B0
  0000000141CD23DD: mov         rax,qword ptr [rbp+268h]
  0000000141CD23E4: test        rax,rax
  0000000141CD23E7: je          0000000141CD2403
  0000000141CD23E9: mov         rcx,qword ptr [rbp+270h]
  0000000141CD23F0: shl         rax,2
  0000000141CD23F4: lea         rdx,[rax+rax*2]
  0000000141CD23F8: mov         r8d,4
  0000000141CD23FE: call        __rust_dealloc
  0000000141CD2403: mov         rdx,qword ptr [rbp+88h]
  0000000141CD240A: test        rdx,rdx
  0000000141CD240D: je          0000000141CD2439
  0000000141CD240F: lea         rax,[rdx*8+17h]
  0000000141CD2417: and         rax,0FFFFFFFFFFFFFFF0h
  0000000141CD241B: add         rdx,rax
  0000000141CD241E: add         rdx,11h
  0000000141CD2422: je          0000000141CD2439
  0000000141CD2424: mov         rcx,qword ptr [rbp+80h]
  0000000141CD242B: sub         rcx,rax
  0000000141CD242E: mov         r8d,10h
  0000000141CD2434: call        __rust_dealloc
  0000000141CD2439: cmp         qword ptr [rbp+210h],0
  0000000141CD2441: je          0000000141CD248E
  0000000141CD2443: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CD2448: mov         rcx,rax
  0000000141CD244B: mov         rax,qword ptr [rax]
  0000000141CD244E: cmp         rax,1
  0000000141CD2452: jne         0000000141CD24F0
  0000000141CD2458: add         rcx,8
  0000000141CD245C: cmp         qword ptr [rcx],0
  0000000141CD2460: jne         0000000141CD2514
  0000000141CD2466: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CD246D: mov         qword ptr [rbp+2D0h],rcx
  0000000141CD2474: add         rcx,8
  0000000141CD2478: mov         rdx,qword ptr [rbp+218h]
  0000000141CD247F: call        _ZN6puffin14ThreadProfiler9end_scope17hbdd5f34d320e5739E
  0000000141CD2484: mov         rax,qword ptr [rbp+2D0h]
  0000000141CD248B: inc         qword ptr [rax]
  0000000141CD248E: movaps      xmm6,xmmword ptr [rbp+330h]
  0000000141CD2495: movaps      xmm7,xmmword ptr [rbp+340h]
  0000000141CD249C: movaps      xmm8,xmmword ptr [rbp+350h]
  0000000141CD24A4: movaps      xmm9,xmmword ptr [rbp+360h]
  0000000141CD24AC: movaps      xmm10,xmmword ptr [rbp+370h]
  0000000141CD24B4: movaps      xmm11,xmmword ptr [rbp+380h]
  0000000141CD24BC: movaps      xmm12,xmmword ptr [rbp+390h]
  0000000141CD24C4: movaps      xmm13,xmmword ptr [rbp+3A0h]
  0000000141CD24CC: movaps      xmm14,xmmword ptr [rbp+3B0h]
  0000000141CD24D4: movaps      xmm15,xmmword ptr [rbp+3C0h]
  0000000141CD24DC: add         rsp,458h
  0000000141CD24E3: pop         rbx
  0000000141CD24E4: pop         rdi
  0000000141CD24E5: pop         rsi
  0000000141CD24E6: pop         r12
  0000000141CD24E8: pop         r13
  0000000141CD24EA: pop         r14
  0000000141CD24EC: pop         r15
  0000000141CD24EE: pop         rbp
  0000000141CD24EF: ret
  0000000141CD24F0: test        rax,rax
  0000000141CD24F3: jne         0000000141CD2508
  0000000141CD24F5: xor         edx,edx
  0000000141CD24F7: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CD24FC: mov         rcx,rax
  0000000141CD24FF: test        rax,rax
  0000000141CD2502: jne         0000000141CD245C
  0000000141CD2508: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.1.llvm.13421880302868465498]
  0000000141CD250F: call        _ZN3std6thread5local18panic_access_error17hcff639665d708376E
  0000000141CD2514: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.3.llvm.13421880302868465498]
  0000000141CD251B: call        _ZN4core4cell22panic_already_borrowed17h54fda569c7a6ddb1E
  0000000141CD2520: mov         r12,r9
  0000000141CD2523: movdqu      xmm1,xmmword ptr [r9+rdx]
  0000000141CD2529: movdqa      xmm2,xmm1
  0000000141CD252D: pcmpeqb     xmm2,xmm0
  0000000141CD2531: pmovmskb    r9d,xmm2
  0000000141CD2536: test        r9d,r9d
  0000000141CD2539: je          0000000141CD2570
  0000000141CD253B: tzcnt       r10d,r9d
  0000000141CD2540: add         r10,rdx
  0000000141CD2543: and         r10,rcx
  0000000141CD2546: neg         r10
  0000000141CD2549: imul        rsi,r10,2A8h
  0000000141CD2550: cmp         rdi,qword ptr [rax+rsi]
  0000000141CD2554: je          0000000141CD25A0
  0000000141CD2556: lea         r10d,[r9-1]
  0000000141CD255A: and         r10w,r9w
  0000000141CD255E: mov         r9d,r10d
  0000000141CD2561: jne         0000000141CD253B
  0000000141CD2563: nop         word ptr cs:[rax+rax]
  0000000141CD2570: pcmpeqb     xmm1,xmm14
  0000000141CD2575: pmovmskb    r9d,xmm1
  0000000141CD257A: test        r9d,r9d
  0000000141CD257D: jne         0000000141CD25AC
  0000000141CD257F: add         rdx,r8
  0000000141CD2582: add         rdx,10h
  0000000141CD2586: add         r8,10h
  0000000141CD258A: and         rdx,rcx
  0000000141CD258D: mov         r9,r12
  0000000141CD2590: jmp         0000000141CD2520
  0000000141CD2592: nop         word ptr cs:[rax+rax]
  0000000141CD25A0: cmp         dword ptr [r12+rsi-48h],2
  0000000141CD25A6: jb          0000000141CD2662
  0000000141CD25AC: cmp         qword ptr [rbp+2D0h],0
  0000000141CD25B4: je          0000000141CD237E
  0000000141CD25BA: test        r14w,r14w
  0000000141CD25BE: jne         0000000141CD25DF
  0000000141CD25C0: movdqa      xmm0,xmmword ptr [r13]
  0000000141CD25C6: pmovmskb    r14d,xmm0
  0000000141CD25CB: add         rbx,0FFFFFFFFFFFFFF80h
  0000000141CD25CF: add         r13,10h
  0000000141CD25D3: cmp         r14d,0FFFFh
  0000000141CD25DA: je          0000000141CD25C0
  0000000141CD25DC: not         r14d
  0000000141CD25DF: mov         eax,r14d
  0000000141CD25E2: lea         r14d,[rax-1]
  0000000141CD25E6: and         r14d,eax
  0000000141CD25E9: dec         qword ptr [rbp+2D0h]
  0000000141CD25F0: tzcnt       eax,eax
  0000000141CD25F4: shl         eax,3
  0000000141CD25F7: mov         rcx,rbx
  0000000141CD25FA: sub         rcx,rax
  0000000141CD25FD: mov         rdi,qword ptr [rcx-8]
  0000000141CD2601: mov         qword ptr [rbp+2C8h],rdi
  0000000141CD2608: mov         rax,qword ptr [rbp+2E0h]
  0000000141CD260F: cmp         qword ptr [rax+18h],0
  0000000141CD2614: je          0000000141CD25AC
  0000000141CD2616: mov         rcx,qword ptr [rbp+250h]
  0000000141CD261D: lea         rdx,[rbp+2C8h]
  0000000141CD2624: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  0000000141CD2629: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD2630: mov         r9,qword ptr [rcx]
  0000000141CD2633: mov         rcx,qword ptr [rcx+8]
  0000000141CD2637: mov         rdx,rcx
  0000000141CD263A: and         rdx,rax
  0000000141CD263D: shr         rax,39h
  0000000141CD2641: movd        xmm0,eax
  0000000141CD2645: punpcklbw   xmm0,xmm0
  0000000141CD2649: pshuflw     xmm0,xmm0,0
  0000000141CD264E: pshufd      xmm0,xmm0,0
  0000000141CD2653: lea         rax,[r9-2A8h]
  0000000141CD265A: xor         r8d,r8d
  0000000141CD265D: jmp         0000000141CD2520
  0000000141CD2662: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD2669: mov         rdx,rdi
  0000000141CD266C: call        _ZN12country_core9resources5walls12public_walls11PublicWalls11roof_entity17h8ce15f3ac3006d60E
  0000000141CD2671: mov         rcx,rax
  0000000141CD2674: shr         rcx,20h
  0000000141CD2678: jne         0000000141CD2689
  0000000141CD267A: mov         dword ptr [rbp+2C0h],0
  0000000141CD2684: jmp         0000000141CD274F
  0000000141CD2689: mov         r8d,eax
  0000000141CD268C: mov         rdi,qword ptr [rbp+300h]
  0000000141CD2693: cmp         r8,qword ptr [rdi+10h]
  0000000141CD2697: mov         r11,qword ptr [rbp+2F0h]
  0000000141CD269E: jae         0000000141CD3710
  0000000141CD26A4: mov         rdx,qword ptr [rdi+8]
  0000000141CD26A8: mov         rcx,rax
  0000000141CD26AB: shr         rcx,20h
  0000000141CD26AF: lea         r9,[r8+r8*4]
  0000000141CD26B3: cmp         dword ptr [rdx+r9*4],ecx
  0000000141CD26B7: jne         0000000141CD3710
  0000000141CD26BD: lea         r9,[rdx+r9*4]
  0000000141CD26C1: mov         edx,dword ptr [r9+4]
  0000000141CD26C5: mov         r10d,0FFFFFFFFh
  0000000141CD26CB: cmp         rdx,r10
  0000000141CD26CE: je          0000000141CD3710
  0000000141CD26D4: xor         r8d,r8d
  0000000141CD26D7: cmp         qword ptr [r11+38h],rdx
  0000000141CD26DB: jbe         0000000141CD3720
  0000000141CD26E1: mov         rcx,qword ptr [r11+28h]
  0000000141CD26E5: mov         r10d,edx
  0000000141CD26E8: shr         r10d,6
  0000000141CD26EC: mov         rcx,qword ptr [rcx+r10*8]
  0000000141CD26F0: bt          rcx,rdx
  0000000141CD26F4: jae         0000000141CD3720
  0000000141CD26FA: mov         eax,dword ptr [r9+0Ch]
  0000000141CD26FE: mov         ecx,dword ptr [r9+10h]
  0000000141CD2702: mov         rdx,qword ptr [rdi+1A8h]
  0000000141CD2709: lea         rax,[rax+rax*8]
  0000000141CD270D: mov         r8,qword ptr [r11+110h]
  0000000141CD2714: mov         r9,qword ptr [rdx+rax*8+18h]
  0000000141CD2719: mov         rax,qword ptr [rdx+rax*8+38h]
  0000000141CD271E: mov         rax,qword ptr [rax+r8*8]
  0000000141CD2722: not         rax
  0000000141CD2725: lea         rax,[rax+rax*2]
  0000000141CD2729: shl         rax,4
  0000000141CD272D: imul        rcx,rcx,58h
  0000000141CD2731: add         rcx,qword ptr [r9+rax+10h]
  0000000141CD2736: call        _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E
  0000000141CD273B: mov         dword ptr [rbp+2C0h],eax
  0000000141CD2741: mov         eax,dword ptr [rbp+2C0h]
  0000000141CD2747: xor         al,1
  0000000141CD2749: mov         dword ptr [rbp+2C0h],eax
  0000000141CD274F: add         r12,rsi
  0000000141CD2752: lea         rcx,[r12-2A0h]
  0000000141CD275A: mov         qword ptr [rbp+310h],r12
  0000000141CD2761: movss       xmm9,dword ptr [r12-20h]
  0000000141CD2768: mov         qword ptr [rbp+298h],rcx
  0000000141CD276F: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  0000000141CD2774: movdqa      xmm8,xmm0
  0000000141CD2779: movzx       eax,byte ptr [rbp+2C0h]
  0000000141CD2780: xorps       xmm0,xmm0
  0000000141CD2783: cvtsi2ss    xmm0,eax
  0000000141CD2787: mov         rcx,qword ptr [rbp+310h]
  0000000141CD278E: lea         rax,[rcx-48h]
  0000000141CD2792: mov         qword ptr [rbp+2D8h],rax
  0000000141CD2799: mulss       xmm0,dword ptr [__real@bf2b851f]
  0000000141CD27A1: addss       xmm8,xmm0
  0000000141CD27A6: movss       dword ptr [rbp+288h],xmm9
  0000000141CD27AF: movss       dword ptr [rbp+28Ch],xmm8
  0000000141CD27B8: mov         rax,qword ptr [rbp+2E0h]
  0000000141CD27BF: mov         r12,qword ptr [rax]
  0000000141CD27C2: mov         rax,qword ptr [rax+18h]
  0000000141CD27C6: mov         qword ptr [rbp+2B8h],rax
  0000000141CD27CD: movdqa      xmm0,xmmword ptr [r12]
  0000000141CD27D3: pmovmskb    esi,xmm0
  0000000141CD27D7: not         esi
  0000000141CD27D9: lea         r15,[r12+10h]
  0000000141CD27DE: lea         rax,[rcx-7]
  0000000141CD27E2: mov         qword ptr [rbp+130h],rax
  0000000141CD27E9: jmp         0000000141CD2860
  0000000141CD27EB: nop         dword ptr [rax+rax]
  0000000141CD27F0: movdqu      xmm1,xmmword ptr [rcx+r8]
  0000000141CD27F6: movdqa      xmm2,xmm1
  0000000141CD27FA: pcmpeqb     xmm2,xmm0
  0000000141CD27FE: pmovmskb    r9d,xmm2
  0000000141CD2803: test        r9d,r9d
  0000000141CD2806: je          0000000141CD2830
  0000000141CD2808: tzcnt       r10d,r9d
  0000000141CD280D: add         r10,r8
  0000000141CD2810: and         r10,rdx
  0000000141CD2813: shl         r10,3
  0000000141CD2817: mov         r11,rcx
  0000000141CD281A: sub         r11,r10
  0000000141CD281D: cmp         rdi,qword ptr [r11-8]
  0000000141CD2821: je          0000000141CD2860
  0000000141CD2823: lea         r10d,[r9-1]
  0000000141CD2827: and         r10w,r9w
  0000000141CD282B: mov         r9d,r10d
  0000000141CD282E: jne         0000000141CD2808
  0000000141CD2830: pcmpeqb     xmm1,xmm14
  0000000141CD2835: pmovmskb    r9d,xmm1
  0000000141CD283A: test        r9d,r9d
  0000000141CD283D: jne         0000000141CD2940
  0000000141CD2843: add         r8,rax
  0000000141CD2846: add         r8,10h
  0000000141CD284A: add         rax,10h
  0000000141CD284E: and         r8,rdx
  0000000141CD2851: jmp         0000000141CD27F0
  0000000141CD2853: nop         word ptr cs:[rax+rax]
  0000000141CD2860: mov         rdx,qword ptr [rbp+2B8h]
  0000000141CD2867: test        rdx,rdx
  0000000141CD286A: je          0000000141CD369C
  0000000141CD2870: test        si,si
  0000000141CD2873: je          0000000141CD2880
  0000000141CD2875: lea         eax,[rsi-1]
  0000000141CD2878: and         eax,esi
  0000000141CD287A: jmp         0000000141CD28A9
  0000000141CD287C: nop         dword ptr [rax]
  0000000141CD2880: movdqa      xmm0,xmmword ptr [r15]
  0000000141CD2885: pmovmskb    esi,xmm0
  0000000141CD2889: add         r12,0FFFFFFFFFFFFD580h
  0000000141CD2890: add         r15,10h
  0000000141CD2894: cmp         esi,0FFFFh
  0000000141CD289A: je          0000000141CD2880
  0000000141CD289C: mov         ecx,0FFFFFFFEh
  0000000141CD28A1: sub         ecx,esi
  0000000141CD28A3: not         esi
  0000000141CD28A5: mov         eax,esi
  0000000141CD28A7: and         eax,ecx
  0000000141CD28A9: mov         ecx,esi
  0000000141CD28AB: tzcnt       ecx,ecx
  0000000141CD28AF: mov         esi,eax
  0000000141CD28B1: dec         rdx
  0000000141CD28B4: mov         qword ptr [rbp+2B8h],rdx
  0000000141CD28BB: neg         rcx
  0000000141CD28BE: imul        rax,rcx,2A8h
  0000000141CD28C5: mov         qword ptr [rbp+2E8h],rax
  0000000141CD28CC: mov         rdi,qword ptr [r12+rax-2A8h]
  0000000141CD28D4: mov         qword ptr [rbp+2B0h],rdi
  0000000141CD28DB: mov         rax,qword ptr [rbp+2C8h]
  0000000141CD28E2: cmp         rax,rdi
  0000000141CD28E5: je          0000000141CD2860
  0000000141CD28EB: jbe         0000000141CD2940
  0000000141CD28ED: cmp         qword ptr [rbp+98h],0
  0000000141CD28F5: je          0000000141CD2940
  0000000141CD28F7: lea         rcx,[rbp+0A0h]
  0000000141CD28FE: lea         rdx,[rbp+2B0h]
  0000000141CD2905: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  0000000141CD290A: mov         rcx,qword ptr [rbp+80h]
  0000000141CD2911: mov         rdx,qword ptr [rbp+88h]
  0000000141CD2918: mov         r8,rdx
  0000000141CD291B: and         r8,rax
  0000000141CD291E: shr         rax,39h
  0000000141CD2922: movd        xmm0,eax
  0000000141CD2926: punpcklbw   xmm0,xmm0
  0000000141CD292A: pshuflw     xmm0,xmm0,0
  0000000141CD292F: pshufd      xmm0,xmm0,0
  0000000141CD2934: xor         eax,eax
  0000000141CD2936: jmp         0000000141CD27F0
  0000000141CD293B: nop         dword ptr [rax+rax]
  0000000141CD2940: mov         rax,qword ptr [rbp+2E8h]
  0000000141CD2947: add         rax,r12
  0000000141CD294A: mov         qword ptr [rbp+320h],rax
  0000000141CD2951: mov         eax,dword ptr [rax-48h]
  0000000141CD2954: cmp         eax,1
  0000000141CD2957: ja          0000000141CD2860
  0000000141CD295D: mov         rcx,qword ptr [rbp+2D8h]
  0000000141CD2964: cmp         dword ptr [rcx],eax
  0000000141CD2966: jne         0000000141CD2A6A
  0000000141CD296C: test        al,1
  0000000141CD296E: je          0000000141CD29FA
  0000000141CD2974: mov         rax,qword ptr [rbp+310h]
  0000000141CD297B: movss       xmm0,dword ptr [rax-44h]
  0000000141CD2980: mov         rax,qword ptr [rbp+320h]
  0000000141CD2987: ucomiss     xmm0,dword ptr [rax-44h]
  0000000141CD298B: jne         0000000141CD2A6A
  0000000141CD2991: jp          0000000141CD2A6A
  0000000141CD2997: mov         rax,qword ptr [rbp+310h]
  0000000141CD299E: movss       xmm0,dword ptr [rax-40h]
  0000000141CD29A3: mov         rax,qword ptr [rbp+320h]
  0000000141CD29AA: ucomiss     xmm0,dword ptr [rax-40h]
  0000000141CD29AE: jne         0000000141CD2A6A
  0000000141CD29B4: jp          0000000141CD2A6A
  0000000141CD29BA: mov         rax,qword ptr [rbp+310h]
  0000000141CD29C1: movss       xmm0,dword ptr [rax-34h]
  0000000141CD29C6: mov         rax,qword ptr [rbp+320h]
  0000000141CD29CD: ucomiss     xmm0,dword ptr [rax-34h]
  0000000141CD29D1: jne         0000000141CD2A6A
  0000000141CD29D7: jp          0000000141CD2A6A
  0000000141CD29DD: mov         rax,qword ptr [rbp+310h]
  0000000141CD29E4: movss       xmm0,dword ptr [rax-30h]
  0000000141CD29E9: mov         rax,qword ptr [rbp+320h]
  0000000141CD29F0: ucomiss     xmm0,dword ptr [rax-30h]
  0000000141CD29F4: jne         0000000141CD2A6A
  0000000141CD29F6: jnp         0000000141CD2A30
  0000000141CD29F8: jmp         0000000141CD2A6A
  0000000141CD29FA: mov         rax,qword ptr [rbp+310h]
  0000000141CD2A01: movss       xmm0,dword ptr [rax-44h]
  0000000141CD2A06: mov         rax,qword ptr [rbp+320h]
  0000000141CD2A0D: ucomiss     xmm0,dword ptr [rax-44h]
  0000000141CD2A11: jne         0000000141CD2A6A
  0000000141CD2A13: jp          0000000141CD2A6A
  0000000141CD2A15: mov         rax,qword ptr [rbp+310h]
  0000000141CD2A1C: movss       xmm0,dword ptr [rax-40h]
  0000000141CD2A21: mov         rax,qword ptr [rbp+320h]
  0000000141CD2A28: ucomiss     xmm0,dword ptr [rax-40h]
  0000000141CD2A2C: jne         0000000141CD2A6A
  0000000141CD2A2E: jp          0000000141CD2A6A
  0000000141CD2A30: mov         rax,qword ptr [rbp+310h]
  0000000141CD2A37: movss       xmm0,dword ptr [rax-3Ch]
  0000000141CD2A3C: mov         rax,qword ptr [rbp+320h]
  0000000141CD2A43: ucomiss     xmm0,dword ptr [rax-3Ch]
  0000000141CD2A47: jne         0000000141CD2A6A
  0000000141CD2A49: jp          0000000141CD2A6A
  0000000141CD2A4B: mov         rax,qword ptr [rbp+310h]
  0000000141CD2A52: movss       xmm0,dword ptr [rax-38h]
  0000000141CD2A57: mov         rax,qword ptr [rbp+320h]
  0000000141CD2A5E: ucomiss     xmm0,dword ptr [rax-38h]
  0000000141CD2A62: jne         0000000141CD2A6A
  0000000141CD2A64: jnp         0000000141CD2860
  0000000141CD2A6A: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD2A71: mov         rdx,rdi
  0000000141CD2A74: call        _ZN12country_core9resources5walls12public_walls11PublicWalls11roof_entity17h8ce15f3ac3006d60E
  0000000141CD2A79: mov         rcx,rax
  0000000141CD2A7C: shr         rcx,20h
  0000000141CD2A80: jne         0000000141CD2A89
  0000000141CD2A82: xor         edi,edi
  0000000141CD2A84: jmp         0000000141CD2B48
  0000000141CD2A89: mov         r8d,eax
  0000000141CD2A8C: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2A93: cmp         r8,qword ptr [rdx+10h]
  0000000141CD2A97: jae         0000000141CD36BF
  0000000141CD2A9D: mov         rdx,qword ptr [rdx+8]
  0000000141CD2AA1: mov         rcx,rax
  0000000141CD2AA4: shr         rcx,20h
  0000000141CD2AA8: lea         r9,[r8+r8*4]
  0000000141CD2AAC: cmp         dword ptr [rdx+r9*4],ecx
  0000000141CD2AB0: jne         0000000141CD36BF
  0000000141CD2AB6: lea         r9,[rdx+r9*4]
  0000000141CD2ABA: mov         edx,dword ptr [r9+4]
  0000000141CD2ABE: mov         r10d,0FFFFFFFFh
  0000000141CD2AC4: cmp         rdx,r10
  0000000141CD2AC7: je          0000000141CD36BF
  0000000141CD2ACD: xor         r8d,r8d
  0000000141CD2AD0: mov         r11,qword ptr [rbp+2F0h]
  0000000141CD2AD7: cmp         qword ptr [r11+38h],rdx
  0000000141CD2ADB: jbe         0000000141CD36CF
  0000000141CD2AE1: mov         rcx,qword ptr [r11+28h]
  0000000141CD2AE5: mov         r10d,edx
  0000000141CD2AE8: shr         r10d,6
  0000000141CD2AEC: mov         rcx,qword ptr [rcx+r10*8]
  0000000141CD2AF0: bt          rcx,rdx
  0000000141CD2AF4: jae         0000000141CD36CF
  0000000141CD2AFA: mov         eax,dword ptr [r9+0Ch]
  0000000141CD2AFE: mov         ecx,dword ptr [r9+10h]
  0000000141CD2B02: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2B09: mov         rdx,qword ptr [rdx+1A8h]
  0000000141CD2B10: lea         rax,[rax+rax*8]
  0000000141CD2B14: mov         r8,qword ptr [r11+110h]
  0000000141CD2B1B: mov         r9,qword ptr [rdx+rax*8+18h]
  0000000141CD2B20: mov         rax,qword ptr [rdx+rax*8+38h]
  0000000141CD2B25: mov         rax,qword ptr [rax+r8*8]
  0000000141CD2B29: not         rax
  0000000141CD2B2C: lea         rax,[rax+rax*2]
  0000000141CD2B30: shl         rax,4
  0000000141CD2B34: imul        rcx,rcx,58h
  0000000141CD2B38: add         rcx,qword ptr [r9+rax+10h]
  0000000141CD2B3D: call        _ZN12country_core9resources5roofs10roof_shape4Roof2ty17h001241e6e27ea1b3E
  0000000141CD2B42: mov         edi,eax
  0000000141CD2B44: xor         dil,1
  0000000141CD2B48: mov         rax,qword ptr [rbp+2E8h]
  0000000141CD2B4F: lea         rcx,[r12+rax]
  0000000141CD2B53: add         rcx,0FFFFFFFFFFFFFD60h
  0000000141CD2B5A: mov         rax,qword ptr [rbp+320h]
  0000000141CD2B61: movss       xmm10,dword ptr [rax-20h]
  0000000141CD2B67: mov         qword ptr [rbp+2E8h],rcx
  0000000141CD2B6E: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  0000000141CD2B73: movdqa      xmm9,xmm0
  0000000141CD2B78: movzx       eax,dil
  0000000141CD2B7C: xorps       xmm0,xmm0
  0000000141CD2B7F: cvtsi2ss    xmm0,eax
  0000000141CD2B83: mulss       xmm0,dword ptr [__real@bf2b851f]
  0000000141CD2B8B: addss       xmm9,xmm0
  0000000141CD2B90: movss       dword ptr [rbp+280h],xmm10
  0000000141CD2B99: movss       dword ptr [rbp+284h],xmm9
  0000000141CD2BA2: lea         rcx,[rbp+288h]
  0000000141CD2BA9: lea         rdx,[rbp+280h]
  0000000141CD2BB0: call        _ZN76_$LT$core..ops..range..Range$LT$f32$GT$$u20$as$u20$utils..RangeIntersect$GT$10intersects17he94120f690a3b6a2E
  0000000141CD2BB5: test        al,al
  0000000141CD2BB7: je          0000000141CD2860
  0000000141CD2BBD: cmp         byte ptr [rbp+2C0h],0
  0000000141CD2BC4: je          0000000141CD2BE9
  0000000141CD2BC6: movaps      xmm0,xmm8
  0000000141CD2BCA: subss       xmm0,xmm9
  0000000141CD2BCF: andps       xmm0,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  0000000141CD2BD6: movss       xmm1,dword ptr [__real@40000000]
  0000000141CD2BDE: ucomiss     xmm1,xmm0
  0000000141CD2BE1: setae       al
  0000000141CD2BE4: and         dil,al
  0000000141CD2BE7: jmp         0000000141CD2BEB
  0000000141CD2BE9: xor         edi,edi
  0000000141CD2BEB: mov         rcx,qword ptr [rbp+298h]
  0000000141CD2BF2: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  0000000141CD2BF7: movaps      xmm10,xmm0
  0000000141CD2BFB: mov         rdx,qword ptr [rbp+2C8h]
  0000000141CD2C02: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD2C09: call        _ZN12country_core9resources5walls12public_walls11PublicWalls11roof_entity17h8ce15f3ac3006d60E
  0000000141CD2C0E: mov         rcx,rax
  0000000141CD2C11: shr         rcx,20h
  0000000141CD2C15: je          0000000141CD2CFC
  0000000141CD2C1B: mov         eax,eax
  0000000141CD2C1D: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2C24: cmp         rax,qword ptr [rdx+10h]
  0000000141CD2C28: jae         0000000141CD2CFC
  0000000141CD2C2E: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2C35: mov         rdx,qword ptr [rdx+8]
  0000000141CD2C39: lea         rax,[rax+rax*4]
  0000000141CD2C3D: cmp         dword ptr [rdx+rax*4],ecx
  0000000141CD2C40: jne         0000000141CD2CFC
  0000000141CD2C46: lea         rax,[rdx+rax*4]
  0000000141CD2C4A: mov         ecx,dword ptr [rax+4]
  0000000141CD2C4D: mov         edx,0FFFFFFFFh
  0000000141CD2C52: cmp         rcx,rdx
  0000000141CD2C55: je          0000000141CD2CFC
  0000000141CD2C5B: mov         rdx,qword ptr [rbp+2F0h]
  0000000141CD2C62: cmp         qword ptr [rdx+38h],rcx
  0000000141CD2C66: jbe         0000000141CD2CFC
  0000000141CD2C6C: mov         rdx,qword ptr [rbp+2F0h]
  0000000141CD2C73: mov         rdx,qword ptr [rdx+28h]
  0000000141CD2C77: mov         r8d,ecx
  0000000141CD2C7A: shr         r8d,6
  0000000141CD2C7E: mov         rdx,qword ptr [rdx+r8*8]
  0000000141CD2C82: bt          rdx,rcx
  0000000141CD2C86: jae         0000000141CD2CFC
  0000000141CD2C88: mov         ecx,dword ptr [rax+0Ch]
  0000000141CD2C8B: mov         edx,dword ptr [rax+10h]
  0000000141CD2C8E: mov         rax,qword ptr [rbp+300h]
  0000000141CD2C95: mov         rax,qword ptr [rax+1A8h]
  0000000141CD2C9C: lea         rcx,[rcx+rcx*8]
  0000000141CD2CA0: mov         r8,qword ptr [rbp+2F0h]
  0000000141CD2CA7: mov         r8,qword ptr [r8+110h]
  0000000141CD2CAE: mov         r9,qword ptr [rax+rcx*8+18h]
  0000000141CD2CB3: mov         rax,qword ptr [rax+rcx*8+38h]
  0000000141CD2CB8: mov         rax,qword ptr [rax+r8*8]
  0000000141CD2CBC: not         rax
  0000000141CD2CBF: lea         rax,[rax+rax*2]
  0000000141CD2CC3: shl         rax,4
  0000000141CD2CC7: mov         rax,qword ptr [r9+rax+10h]
  0000000141CD2CCC: imul        rcx,rdx,58h
  0000000141CD2CD0: mov         edx,dword ptr [rax+rcx+40h]
  0000000141CD2CD4: cmp         dl,3
  0000000141CD2CD7: mov         r8d,0
  0000000141CD2CDD: cmove       edx,r8d
  0000000141CD2CE1: test        dl,dl
  0000000141CD2CE3: je          0000000141CD2CFC
  0000000141CD2CE5: movzx       edx,dl
  0000000141CD2CE8: xorps       xmm11,xmm11
  0000000141CD2CEC: cmp         edx,1
  0000000141CD2CEF: je          0000000141CD2D19
  0000000141CD2CF1: add         rax,rcx
  0000000141CD2CF4: test        byte ptr [rax+41h],1
  0000000141CD2CF8: jne         0000000141CD2D10
  0000000141CD2CFA: jmp         0000000141CD2D19
  0000000141CD2CFC: mov         rcx,qword ptr [rbp+130h]
  0000000141CD2D03: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  0000000141CD2D08: xorps       xmm11,xmm11
  0000000141CD2D0C: test        al,al
  0000000141CD2D0E: je          0000000141CD2D19
  0000000141CD2D10: movss       xmm11,dword ptr [__real@3dcccccd]
  0000000141CD2D19: addss       xmm10,xmm7
  0000000141CD2D1E: addss       xmm10,xmm6
  0000000141CD2D23: addss       xmm10,xmm15
  0000000141CD2D28: mov         rcx,qword ptr [rbp+2E8h]
  0000000141CD2D2F: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  0000000141CD2D34: movaps      xmm9,xmm0
  0000000141CD2D38: mov         rdx,qword ptr [rbp+2B0h]
  0000000141CD2D3F: mov         rcx,qword ptr [rbp+2E0h]
  0000000141CD2D46: call        _ZN12country_core9resources5walls12public_walls11PublicWalls11roof_entity17h8ce15f3ac3006d60E
  0000000141CD2D4B: mov         rcx,rax
  0000000141CD2D4E: shr         rcx,20h
  0000000141CD2D52: je          0000000141CD2E38
  0000000141CD2D58: mov         eax,eax
  0000000141CD2D5A: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2D61: cmp         rax,qword ptr [rdx+10h]
  0000000141CD2D65: jae         0000000141CD2E38
  0000000141CD2D6B: mov         rdx,qword ptr [rbp+300h]
  0000000141CD2D72: mov         rdx,qword ptr [rdx+8]
  0000000141CD2D76: lea         rax,[rax+rax*4]
  0000000141CD2D7A: cmp         dword ptr [rdx+rax*4],ecx
  0000000141CD2D7D: jne         0000000141CD2E38
  0000000141CD2D83: lea         rax,[rdx+rax*4]
  0000000141CD2D87: mov         ecx,dword ptr [rax+4]
  0000000141CD2D8A: mov         edx,0FFFFFFFFh
  0000000141CD2D8F: cmp         rcx,rdx
  0000000141CD2D92: je          0000000141CD2E38
  0000000141CD2D98: mov         rdx,qword ptr [rbp+2F0h]
  0000000141CD2D9F: cmp         qword ptr [rdx+38h],rcx
  0000000141CD2DA3: jbe         0000000141CD2E38
  0000000141CD2DA9: mov         rdx,qword ptr [rbp+2F0h]
  0000000141CD2DB0: mov         rdx,qword ptr [rdx+28h]
  0000000141CD2DB4: mov         r8d,ecx
  0000000141CD2DB7: shr         r8d,6
  0000000141CD2DBB: mov         rdx,qword ptr [rdx+r8*8]
  0000000141CD2DBF: bt          rdx,rcx
  0000000141CD2DC3: jae         0000000141CD2E38
  0000000141CD2DC5: mov         ecx,dword ptr [rax+0Ch]
  0000000141CD2DC8: mov         edx,dword ptr [rax+10h]
  0000000141CD2DCB: mov         rax,qword ptr [rbp+300h]
  0000000141CD2DD2: mov         rax,qword ptr [rax+1A8h]
  0000000141CD2DD9: lea         rcx,[rcx+rcx*8]
  0000000141CD2DDD: mov         r8,qword ptr [rbp+2F0h]
  0000000141CD2DE4: mov         r8,qword ptr [r8+110h]
  0000000141CD2DEB: mov         r9,qword ptr [rax+rcx*8+18h]
  0000000141CD2DF0: mov         rax,qword ptr [rax+rcx*8+38h]
  0000000141CD2DF5: mov         rax,qword ptr [rax+r8*8]
  0000000141CD2DF9: not         rax
  0000000141CD2DFC: lea         rax,[rax+rax*2]
  0000000141CD2E00: shl         rax,4
  0000000141CD2E04: mov         rax,qword ptr [r9+rax+10h]
  0000000141CD2E09: imul        rcx,rdx,58h
  0000000141CD2E0D: mov         edx,dword ptr [rax+rcx+40h]
  0000000141CD2E11: cmp         dl,3
  0000000141CD2E14: mov         r8d,0
  0000000141CD2E1A: cmove       edx,r8d
  0000000141CD2E1E: test        dl,dl
  0000000141CD2E20: je          0000000141CD2E38
  0000000141CD2E22: movzx       edx,dl
  0000000141CD2E25: xorps       xmm0,xmm0
  0000000141CD2E28: cmp         edx,1
  0000000141CD2E2B: je          0000000141CD2E57
  0000000141CD2E2D: add         rax,rcx
  0000000141CD2E30: test        byte ptr [rax+41h],1
  0000000141CD2E34: jne         0000000141CD2E4F
  0000000141CD2E36: jmp         0000000141CD2E57
  0000000141CD2E38: mov         rax,qword ptr [rbp+320h]
  0000000141CD2E3F: lea         rcx,[rax-7]
  0000000141CD2E43: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  0000000141CD2E48: xorps       xmm0,xmm0
  0000000141CD2E4B: test        al,al
  0000000141CD2E4D: je          0000000141CD2E57
  0000000141CD2E4F: movss       xmm0,dword ptr [__real@3dcccccd]
  0000000141CD2E57: subss       xmm10,xmm11
  0000000141CD2E5C: addss       xmm9,xmm7
  0000000141CD2E61: addss       xmm9,xmm6
  0000000141CD2E66: addss       xmm9,xmm15
  0000000141CD2E6B: subss       xmm9,xmm0
  0000000141CD2E70: movss       dword ptr [rbp+208h],xmm10
  0000000141CD2E79: mov         dword ptr [rbp+20Ch],461C4000h
  0000000141CD2E83: movss       dword ptr [rbp+200h],xmm9
  0000000141CD2E8C: mov         dword ptr [rbp+204h],461C4000h
  0000000141CD2E96: mov         rax,qword ptr [rbp+128h]
  0000000141CD2E9D: mov         qword ptr [rbp+28h],rax
  0000000141CD2EA1: mov         rax,qword ptr [rbp+298h]
  0000000141CD2EA8: mov         qword ptr [rbp+30h],rax
  0000000141CD2EAC: lea         rax,[rbp+288h]
  0000000141CD2EB3: mov         qword ptr [rbp+38h],rax
  0000000141CD2EB7: lea         rax,[rbp+28Ch]
  0000000141CD2EBE: mov         qword ptr [rbp+40h],rax
  0000000141CD2EC2: lea         rax,[rbp+280h]
  0000000141CD2EC9: mov         qword ptr [rbp+48h],rax
  0000000141CD2ECD: lea         rax,[rbp+284h]
  0000000141CD2ED4: mov         qword ptr [rbp+50h],rax
  0000000141CD2ED8: mov         rax,qword ptr [rbp+258h]
  0000000141CD2EDF: mov         qword ptr [rbp+58h],rax
  0000000141CD2EE3: lea         rax,[rbp+2C8h]
  0000000141CD2EEA: mov         qword ptr [rbp+60h],rax
  0000000141CD2EEE: lea         rax,[rbp+2B0h]
  0000000141CD2EF5: mov         qword ptr [rbp+68h],rax
  0000000141CD2EF9: mov         rax,qword ptr [rbp+260h]
  0000000141CD2F00: mov         qword ptr [rbp+70h],rax
  0000000141CD2F04: mov         rax,qword ptr [rbp+468h]
  0000000141CD2F0B: mov         qword ptr [rbp+78h],rax
  0000000141CD2F0F: mov         qword ptr [rbp+278h],0
  0000000141CD2F1A: lea         rcx,[rbp+220h]
  0000000141CD2F21: mov         rdx,qword ptr [rbp+2D8h]
  0000000141CD2F28: movdqa      xmm2,xmm12
  0000000141CD2F2D: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters6expand17h1827f25d515a6505E
  0000000141CD2F32: mov         rdx,qword ptr [rbp+320h]
  0000000141CD2F39: add         rdx,0FFFFFFFFFFFFFFB8h
  0000000141CD2F3D: lea         rcx,[rbp+160h]
  0000000141CD2F44: mov         qword ptr [rbp+320h],rdx
  0000000141CD2F4B: movdqa      xmm2,xmm12
  0000000141CD2F50: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters6expand17h1827f25d515a6505E
  0000000141CD2F55: lea         rax,[rbp+268h]
  0000000141CD2F5C: mov         qword ptr [rsp+20h],rax
  0000000141CD2F61: lea         rcx,[rbp+220h]
  0000000141CD2F68: lea         rdx,[rbp+160h]
  0000000141CD2F6F: lea         r8,[rbp+28h]
  0000000141CD2F73: mov         r9d,edi
  0000000141CD2F76: call        _ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners16intersect_shapes17h25a4dbb3a7621abfE
  0000000141CD2F7B: test        dil,dil
  0000000141CD2F7E: je          0000000141CD2860
  0000000141CD2F84: lea         rcx,[rbp-2Ch]
  0000000141CD2F88: mov         rdx,qword ptr [rbp+320h]
  0000000141CD2F8F: movaps      xmm2,xmm13
  0000000141CD2F93: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters6expand17h1827f25d515a6505E
  0000000141CD2F98: movups      xmm0,xmmword ptr [rbp-2Ch]
  0000000141CD2F9C: movdqu      xmm1,xmmword ptr [rbp-20h]
  0000000141CD2FA1: movdqu      xmmword ptr [rbp+16Ch],xmm1
  0000000141CD2FA9: movaps      xmmword ptr [rbp+160h],xmm0
  0000000141CD2FB0: lea         rcx,[rbp+160h]
  0000000141CD2FB7: mov         rdx,qword ptr [rbp+2D8h]
  0000000141CD2FBE: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters14contains_shape17hab2d7e6f21c8a5a4E
  0000000141CD2FC3: test        al,al
  0000000141CD2FC5: je          0000000141CD30C1
  0000000141CD2FCB: mov         rcx,qword ptr [rbp+2D8h]
  0000000141CD2FD2: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters9perimeter17h9756eeb647df630aE
  0000000141CD2FD7: movaps      xmm1,xmm10
  0000000141CD2FDB: addss       xmm1,dword ptr [__real@461c4000]
  0000000141CD2FE3: movss       xmm2,dword ptr [__real@3f000000]
  0000000141CD2FEB: mulss       xmm1,xmm2
  0000000141CD2FEF: mulss       xmm0,xmm2
  0000000141CD2FF3: call        _ZN101_$LT$$LP$$RP$$u20$as$u20$country_core..resources..walls..decorator_storage..DecoAddrChangeTracker$GT$6insert17h37bcd7d6ada1fd09E
  0000000141CD2FF8: movss       dword ptr [rbp+308h],xmm0
  0000000141CD3000: movss       dword ptr [rbp+2F8h],xmm1
  0000000141CD3008: mov         rcx,qword ptr [rbp+2D8h]
  0000000141CD300F: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters9perimeter17h9756eeb647df630aE
  0000000141CD3014: movaps      xmm11,xmm0
  0000000141CD3018: mov         rax,qword ptr [rbp+2C8h]
  0000000141CD301F: mov         qword ptr [rbp+2A0h],rax
  0000000141CD3026: mov         rax,qword ptr [rbp+2B0h]
  0000000141CD302D: mov         qword ptr [rbp+2A8h],rax
  0000000141CD3034: mov         rax,qword ptr [rbp+450h]
  0000000141CD303B: mov         rdi,qword ptr [rax+10h]
  0000000141CD303F: cmp         rdi,qword ptr [rax]
  0000000141CD3042: jne         0000000141CD3057
  0000000141CD3044: mov         rcx,qword ptr [rbp+450h]
  0000000141CD304B: lea         rdx,[142F336A0h]
  0000000141CD3052: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0325531c2a91cae0E
  0000000141CD3057: movss       xmm0,dword ptr [__real@461c4000]
  0000000141CD305F: subss       xmm0,xmm10
  0000000141CD3064: mov         rdx,qword ptr [rbp+450h]
  0000000141CD306B: mov         rax,qword ptr [rdx+8]
  0000000141CD306F: lea         rcx,[rdi+rdi*4]
  0000000141CD3073: mov         r8,qword ptr [rbp+2A0h]
  0000000141CD307A: mov         qword ptr [rax+rcx*8],r8
  0000000141CD307E: mov         r8,qword ptr [rbp+2A8h]
  0000000141CD3085: mov         qword ptr [rax+rcx*8+8],r8
  0000000141CD308A: movss       xmm1,dword ptr [rbp+308h]
  0000000141CD3092: movss       dword ptr [rax+rcx*8+10h],xmm1
  0000000141CD3098: movd        xmm1,dword ptr [rbp+2F8h]
  0000000141CD30A0: movd        dword ptr [rax+rcx*8+14h],xmm1
  0000000141CD30A6: movss       dword ptr [rax+rcx*8+18h],xmm11
  0000000141CD30AD: movss       dword ptr [rax+rcx*8+1Ch],xmm0
  0000000141CD30B3: mov         byte ptr [rax+rcx*8+20h],1
  0000000141CD30B8: inc         rdi
  0000000141CD30BB: mov         qword ptr [rdx+10h],rdi
  0000000141CD30BF: jmp         0000000141CD3103
  0000000141CD30C1: mov         r8,qword ptr [rbp+278h]
  0000000141CD30C8: test        r8,r8
  0000000141CD30CB: je          0000000141CD3103
  0000000141CD30CD: mov         r9,qword ptr [rbp+270h]
  0000000141CD30D4: lea         rax,[r8+r8*2]
  0000000141CD30D8: lea         r10,[r9+rax*4]
  0000000141CD30DC: lea         rax,[r8*4]
  0000000141CD30E4: lea         rax,[rax+rax*2]
  0000000141CD30E8: xor         edx,edx
  0000000141CD30EA: xor         ecx,ecx
  0000000141CD30EC: cmp         dword ptr [r9+rdx],0
  0000000141CD30F1: je          0000000141CD3285
  0000000141CD30F7: inc         rcx
  0000000141CD30FA: add         rdx,0Ch
  0000000141CD30FE: cmp         rax,rdx
  0000000141CD3101: jne         0000000141CD30EC
  0000000141CD3103: lea         rcx,[rbp-48h]
  0000000141CD3107: mov         rdx,qword ptr [rbp+2D8h]
  0000000141CD310E: movaps      xmm2,xmm13
  0000000141CD3112: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters6expand17h1827f25d515a6505E
  0000000141CD3117: movups      xmm0,xmmword ptr [rbp-48h]
  0000000141CD311B: movdqu      xmm1,xmmword ptr [rbp-3Ch]
  0000000141CD3120: movdqu      xmmword ptr [rbp+16Ch],xmm1
  0000000141CD3128: movaps      xmmword ptr [rbp+160h],xmm0
  0000000141CD312F: lea         rcx,[rbp+160h]
  0000000141CD3136: mov         rdx,qword ptr [rbp+320h]
  0000000141CD313D: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters14contains_shape17hab2d7e6f21c8a5a4E
  0000000141CD3142: test        al,al
  0000000141CD3144: je          0000000141CD3238
  0000000141CD314A: mov         rcx,qword ptr [rbp+320h]
  0000000141CD3151: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters9perimeter17h9756eeb647df630aE
  0000000141CD3156: movaps      xmm1,xmm9
  0000000141CD315A: addss       xmm1,dword ptr [__real@461c4000]
  0000000141CD3162: movss       xmm2,dword ptr [__real@3f000000]
  0000000141CD316A: mulss       xmm1,xmm2
  0000000141CD316E: mulss       xmm0,xmm2
  0000000141CD3172: call        _ZN101_$LT$$LP$$RP$$u20$as$u20$country_core..resources..walls..decorator_storage..DecoAddrChangeTracker$GT$6insert17h37bcd7d6ada1fd09E
  0000000141CD3177: movss       dword ptr [rbp+2E8h],xmm0
  0000000141CD317F: movaps      xmm11,xmm1
  0000000141CD3183: mov         rcx,qword ptr [rbp+320h]
  0000000141CD318A: call        _ZN12country_core7systems4wall15valid_enclosure15ShapeParameters9perimeter17h9756eeb647df630aE
  0000000141CD318F: movaps      xmm10,xmm0
  0000000141CD3193: mov         rax,qword ptr [rbp+2B0h]
  0000000141CD319A: mov         qword ptr [rbp+320h],rax
  0000000141CD31A1: mov         rax,qword ptr [rbp+2C8h]
  0000000141CD31A8: mov         qword ptr [rbp+308h],rax
  0000000141CD31AF: mov         rax,qword ptr [rbp+450h]
  0000000141CD31B6: mov         rdi,qword ptr [rax+10h]
  0000000141CD31BA: cmp         rdi,qword ptr [rax]
  0000000141CD31BD: jne         0000000141CD31D2
  0000000141CD31BF: mov         rcx,qword ptr [rbp+450h]
  0000000141CD31C6: lea         rdx,[142F336D0h]
  0000000141CD31CD: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0325531c2a91cae0E
  0000000141CD31D2: movss       xmm0,dword ptr [__real@461c4000]
  0000000141CD31DA: subss       xmm0,xmm9
  0000000141CD31DF: mov         rdx,qword ptr [rbp+450h]
  0000000141CD31E6: mov         rax,qword ptr [rdx+8]
  0000000141CD31EA: lea         rcx,[rdi+rdi*4]
  0000000141CD31EE: mov         r8,qword ptr [rbp+320h]
  0000000141CD31F5: mov         qword ptr [rax+rcx*8],r8
  0000000141CD31F9: mov         r8,qword ptr [rbp+308h]
  0000000141CD3200: mov         qword ptr [rax+rcx*8+8],r8
  0000000141CD3205: movd        xmm1,dword ptr [rbp+2E8h]
  0000000141CD320D: movd        dword ptr [rax+rcx*8+10h],xmm1
  0000000141CD3213: movss       dword ptr [rax+rcx*8+14h],xmm11
  0000000141CD321A: movss       dword ptr [rax+rcx*8+18h],xmm10
  0000000141CD3221: movss       dword ptr [rax+rcx*8+1Ch],xmm0
  0000000141CD3227: mov         byte ptr [rax+rcx*8+20h],1
  0000000141CD322C: inc         rdi
  0000000141CD322F: mov         qword ptr [rdx+10h],rdi
  0000000141CD3233: jmp         0000000141CD2860
  0000000141CD3238: mov         rax,qword ptr [rbp+278h]
  0000000141CD323F: test        rax,rax
  0000000141CD3242: je          0000000141CD2860
  0000000141CD3248: mov         rcx,qword ptr [rbp+270h]
  0000000141CD324F: lea         rdx,[rax+rax*2]
  0000000141CD3253: lea         rdx,[rcx+rdx*4]
  0000000141CD3257: lea         r8,[rax*4]
  0000000141CD325F: lea         r9,[r8+r8*2]
  0000000141CD3263: xor         r10d,r10d
  0000000141CD3266: xor         r8d,r8d
  0000000141CD3269: cmp         dword ptr [rcx+r10],1
  0000000141CD326E: je          0000000141CD34E2
  0000000141CD3274: inc         r8
  0000000141CD3277: add         r10,0Ch
  0000000141CD327B: cmp         r9,r10
  0000000141CD327E: jne         0000000141CD3269
  0000000141CD3280: jmp         0000000141CD2860
  0000000141CD3285: lea         rax,[r9+0Ch]
  0000000141CD3289: mov         qword ptr [rbp+150h],rax
  0000000141CD3290: xor         edi,edi
  0000000141CD3292: mov         qword ptr [rbp+2F8h],r9
  0000000141CD3299: mov         qword ptr [rbp+308h],r10
  0000000141CD32A0: mov         qword ptr [rbp+158h],r8
  0000000141CD32A7: mov         r11,r8
  0000000141CD32AA: mov         qword ptr [rbp+2A8h],r9
  0000000141CD32B1: mov         qword ptr [rbp+290h],r10
  0000000141CD32B8: mov         eax,edi
  0000000141CD32BA: jmp         0000000141CD3322
  0000000141CD32BC: mov         rdx,qword ptr [rbp+450h]
  0000000141CD32C3: mov         rax,qword ptr [rdx+8]
  0000000141CD32C7: mov         r9,qword ptr [rbp+138h]
  0000000141CD32CE: lea         rcx,[r9+r9*4]
  0000000141CD32D2: mov         r8,qword ptr [rbp+140h]
  0000000141CD32D9: mov         qword ptr [rax+rcx*8],r8
  0000000141CD32DD: mov         r8,qword ptr [rbp+148h]
  0000000141CD32E4: mov         qword ptr [rax+rcx*8+8],r8
  0000000141CD32E9: movups      xmm0,xmmword ptr [rbp+160h]
  0000000141CD32F0: movups      xmmword ptr [rax+rcx*8+10h],xmm0
  0000000141CD32F5: movzx       r8d,byte ptr [rbp+31Fh]
  0000000141CD32FD: mov         byte ptr [rax+rcx*8+20h],r8b
  0000000141CD3302: inc         r9
  0000000141CD3305: mov         qword ptr [rdx+10h],r9
  0000000141CD3309: xor         ecx,ecx
  0000000141CD330B: mov         eax,edi
  0000000141CD330D: mov         r9,qword ptr [rbp+2A8h]
  0000000141CD3314: mov         r10,qword ptr [rbp+290h]
  0000000141CD331B: mov         r11,qword ptr [rbp+2A0h]
  0000000141CD3322: test        r11,r11
  0000000141CD3325: je          0000000141CD3103
  0000000141CD332B: test        rcx,rcx
  0000000141CD332E: jne         0000000141CD3418
  0000000141CD3334: mov         rcx,qword ptr [rbp+308h]
  0000000141CD333B: mov         r8,qword ptr [rbp+2F8h]
  0000000141CD3342: cmp         r8,rcx
  0000000141CD3345: cmove       r8,r9
  0000000141CD3349: cmove       rcx,r10
  0000000141CD334D: mov         qword ptr [rbp+308h],rcx
  0000000141CD3354: mov         rdx,r8
  0000000141CD3357: lea         rcx,[r8+0Ch]
  0000000141CD335B: mov         qword ptr [rbp+2F8h],rcx
  0000000141CD3362: dec         r11
  0000000141CD3365: cmp         dword ptr [rdx],0
  0000000141CD3368: sete        dil
  0000000141CD336C: je          0000000141CD34D1
  0000000141CD3372: mov         ecx,0
  0000000141CD3377: test        al,1
  0000000141CD3379: mov         eax,edi
  0000000141CD337B: je          0000000141CD3322
  0000000141CD337D: mov         qword ptr [rbp+2A0h],r11
  0000000141CD3384: movaps      xmm0,xmmword ptr [rbp+10h]
  0000000141CD3388: movhps      xmm0,qword ptr [rdx+4]
  0000000141CD338C: movups      xmmword ptr [rbp+220h],xmm0
  0000000141CD3393: lea         rcx,[rbp+160h]
  0000000141CD339A: mov         rdx,qword ptr [rbp+298h]
  0000000141CD33A1: lea         r8,[rbp+220h]
  0000000141CD33A8: lea         r9,[rbp+208h]
  0000000141CD33AF: call        _ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners30add_hole_at_shape_intersection17heb8e784f3b778c9cE
  0000000141CD33B4: movzx       eax,byte ptr [rbp+170h]
  0000000141CD33BB: cmp         al,5
  0000000141CD33BD: je          0000000141CD3309
  0000000141CD33C3: mov         byte ptr [rbp+31Fh],al
  0000000141CD33C9: mov         rax,qword ptr [rbp+2C8h]
  0000000141CD33D0: mov         qword ptr [rbp+140h],rax
  0000000141CD33D7: mov         rax,qword ptr [rbp+2B0h]
  0000000141CD33DE: mov         qword ptr [rbp+148h],rax
  0000000141CD33E5: mov         rax,qword ptr [rbp+450h]
  0000000141CD33EC: mov         rcx,qword ptr [rax+10h]
  0000000141CD33F0: mov         qword ptr [rbp+138h],rcx
  0000000141CD33F7: cmp         rcx,qword ptr [rax]
  0000000141CD33FA: jne         0000000141CD32BC
  0000000141CD3400: mov         rcx,qword ptr [rbp+450h]
  0000000141CD3407: lea         rdx,[142F336B8h]
  0000000141CD340E: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0325531c2a91cae0E
  0000000141CD3413: jmp         0000000141CD32BC
  0000000141CD3418: mov         rdx,qword ptr [rbp+308h]
  0000000141CD341F: sub         rdx,qword ptr [rbp+2F8h]
  0000000141CD3426: shr         rdx,2
  0000000141CD342A: mov         r8,0AAAAAAAAAAAAAAABh
  0000000141CD3434: imul        rdx,r8
  0000000141CD3438: cmp         rcx,rdx
  0000000141CD343B: cmovb       rdx,rcx
  0000000141CD343F: sub         rcx,rdx
  0000000141CD3442: je          0000000141CD3472
  0000000141CD3444: mov         r8,qword ptr [rbp+158h]
  0000000141CD344B: cmp         rcx,r8
  0000000141CD344E: mov         rdx,r8
  0000000141CD3451: cmovb       rdx,rcx
  0000000141CD3455: sub         rcx,rdx
  0000000141CD3458: jne         0000000141CD344B
  0000000141CD345A: lea         rcx,[rdx+rdx*2]
  0000000141CD345E: mov         rdx,qword ptr [rbp+2A8h]
  0000000141CD3465: lea         rcx,[rdx+rcx*4]
  0000000141CD3469: mov         r8,qword ptr [rbp+290h]
  0000000141CD3470: jmp         0000000141CD3488
  0000000141CD3472: lea         rcx,[rdx+rdx*2]
  0000000141CD3476: mov         rdx,qword ptr [rbp+2F8h]
  0000000141CD347D: lea         rcx,[rdx+rcx*4]
  0000000141CD3481: mov         r8,qword ptr [rbp+308h]
  0000000141CD3488: mov         r9,qword ptr [rbp+2A8h]
  0000000141CD348F: mov         rdx,r9
  0000000141CD3492: mov         r10,qword ptr [rbp+290h]
  0000000141CD3499: mov         qword ptr [rbp+308h],r10
  0000000141CD34A0: mov         rdi,qword ptr [rbp+150h]
  0000000141CD34A7: mov         qword ptr [rbp+2F8h],rdi
  0000000141CD34AE: cmp         rcx,r8
  0000000141CD34B1: je          0000000141CD3362
  0000000141CD34B7: lea         rdx,[rcx+0Ch]
  0000000141CD34BB: mov         qword ptr [rbp+2F8h],rdx
  0000000141CD34C2: mov         rdx,rcx
  0000000141CD34C5: mov         qword ptr [rbp+308h],r8
  0000000141CD34CC: jmp         0000000141CD3362
  0000000141CD34D1: movq        xmm0,mmword ptr [rdx+4]
  0000000141CD34D6: movdqa      xmmword ptr [rbp+10h],xmm0
  0000000141CD34DB: xor         ecx,ecx
  0000000141CD34DD: jmp         0000000141CD32B8
  0000000141CD34E2: mov         qword ptr [rbp+160h],rcx
  0000000141CD34E9: mov         qword ptr [rbp+168h],rdx
  0000000141CD34F0: mov         qword ptr [rbp+170h],rcx
  0000000141CD34F7: mov         qword ptr [rbp+178h],rdx
  0000000141CD34FE: mov         qword ptr [rbp+180h],r8
  0000000141CD3505: xor         ecx,ecx
  0000000141CD3507: test        rax,rax
  0000000141CD350A: jne         0000000141CD352B
  0000000141CD350C: jmp         0000000141CD2860
  0000000141CD3511: movq        xmm0,mmword ptr [rax+4]
  0000000141CD3516: movdqa      xmmword ptr [rbp],xmm0
  0000000141CD351B: mov         rax,qword ptr [rbp+188h]
  0000000141CD3522: test        rax,rax
  0000000141CD3525: je          0000000141CD2860
  0000000141CD352B: mov         edi,ecx
  0000000141CD352D: dec         rax
  0000000141CD3530: mov         qword ptr [rbp+188h],rax
  0000000141CD3537: mov         rdx,qword ptr [rbp+180h]
  0000000141CD353E: test        rdx,rdx
  0000000141CD3541: jne         0000000141CD3677
  0000000141CD3547: mov         rax,qword ptr [rbp+170h]
  0000000141CD354E: cmp         rax,qword ptr [rbp+178h]
  0000000141CD3555: jne         0000000141CD3575
  0000000141CD3557: mov         rax,qword ptr [rbp+160h]
  0000000141CD355E: mov         rcx,qword ptr [rbp+168h]
  0000000141CD3565: mov         qword ptr [rbp+178h],rcx
  0000000141CD356C: cmp         rax,rcx
  0000000141CD356F: je          0000000141CD2860
  0000000141CD3575: lea         rcx,[rax+0Ch]
  0000000141CD3579: mov         qword ptr [rbp+170h],rcx
  0000000141CD3580: cmp         dword ptr [rax],1
  0000000141CD3583: sete        cl
  0000000141CD3586: je          0000000141CD3511
  0000000141CD3588: test        dil,1
  0000000141CD358C: je          0000000141CD3662
  0000000141CD3592: mov         dword ptr [rbp+320h],ecx
  0000000141CD3598: movaps      xmm0,xmmword ptr [rbp]
  0000000141CD359C: movhps      xmm0,qword ptr [rax+4]
  0000000141CD35A0: movups      xmmword ptr [rbp-10h],xmm0
  0000000141CD35A4: lea         rcx,[rbp+220h]
  0000000141CD35AB: mov         rdx,qword ptr [rbp+2E8h]
  0000000141CD35B2: lea         r8,[rbp-10h]
  0000000141CD35B6: lea         r9,[rbp+200h]
  0000000141CD35BD: call        _ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners30add_hole_at_shape_intersection17heb8e784f3b778c9cE
  0000000141CD35C2: movzx       eax,byte ptr [rbp+230h]
  0000000141CD35C9: cmp         al,5
  0000000141CD35CB: je          0000000141CD365C
  0000000141CD35D1: mov         byte ptr [rbp+308h],al
  0000000141CD35D7: mov         rax,qword ptr [rbp+2B0h]
  0000000141CD35DE: mov         qword ptr [rbp+2F8h],rax
  0000000141CD35E5: mov         rax,qword ptr [rbp+2C8h]
  0000000141CD35EC: mov         qword ptr [rbp+2A0h],rax
  0000000141CD35F3: mov         rax,qword ptr [rbp+450h]
  0000000141CD35FA: mov         rdi,qword ptr [rax+10h]
  0000000141CD35FE: cmp         rdi,qword ptr [rax]
  0000000141CD3601: jne         0000000141CD3616
  0000000141CD3603: mov         rcx,qword ptr [rbp+450h]
  0000000141CD360A: lea         rdx,[142F336E8h]
  0000000141CD3611: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h0325531c2a91cae0E
  0000000141CD3616: mov         rdx,qword ptr [rbp+450h]
  0000000141CD361D: mov         rax,qword ptr [rdx+8]
  0000000141CD3621: lea         rcx,[rdi+rdi*4]
  0000000141CD3625: mov         r8,qword ptr [rbp+2F8h]
  0000000141CD362C: mov         qword ptr [rax+rcx*8],r8
  0000000141CD3630: mov         r8,qword ptr [rbp+2A0h]
  0000000141CD3637: mov         qword ptr [rax+rcx*8+8],r8
  0000000141CD363C: movups      xmm0,xmmword ptr [rbp+220h]
  0000000141CD3643: movups      xmmword ptr [rax+rcx*8+10h],xmm0
  0000000141CD3648: movzx       r8d,byte ptr [rbp+308h]
  0000000141CD3650: mov         byte ptr [rax+rcx*8+20h],r8b
  0000000141CD3655: inc         rdi
  0000000141CD3658: mov         qword ptr [rdx+10h],rdi
  0000000141CD365C: mov         ecx,dword ptr [rbp+320h]
  0000000141CD3662: mov         rax,qword ptr [rbp+188h]
  0000000141CD3669: test        rax,rax
  0000000141CD366C: jne         0000000141CD352B
  0000000141CD3672: jmp         0000000141CD2860
  0000000141CD3677: mov         qword ptr [rbp+180h],0
  0000000141CD3682: lea         rcx,[rbp+160h]
  0000000141CD3689: call        0000000141CD1540
  0000000141CD368E: test        rax,rax
  0000000141CD3691: jne         0000000141CD3580
  0000000141CD3697: jmp         0000000141CD2860
  0000000141CD369C: mov         r15,qword ptr [rbp+450h]
  0000000141CD36A3: mov         rax,qword ptr [rbp+2D0h]
  0000000141CD36AA: mov         qword ptr [rbp+2D0h],rax
  0000000141CD36B1: test        rax,rax
  0000000141CD36B4: jne         0000000141CD25BA
  0000000141CD36BA: jmp         0000000141CD237E
  0000000141CD36BF: shl         rcx,20h
  0000000141CD36C3: or          rcx,r8
  0000000141CD36C6: mov         r8d,1
  0000000141CD36CC: mov         rax,rcx
  0000000141CD36CF: mov         dword ptr [rbp+160h],r8d
  0000000141CD36D6: mov         dword ptr [rbp+164h],edx
  0000000141CD36DC: mov         qword ptr [rbp+168h],rax
  0000000141CD36E3: lea         rax,[142F33688h]
  0000000141CD36EA: mov         qword ptr [rsp+20h],rax
  0000000141CD36EF: lea         rcx,[142F33400h]
  0000000141CD36F6: lea         r9,[142F333E0h]
  0000000141CD36FD: lea         r8,[rbp+160h]
  0000000141CD3704: mov         edx,2Bh
  0000000141CD3709: call        _ZN4core6result13unwrap_failed17h96d74ae09566f4b3E
  0000000141CD370E: jmp         0000000141CD375F
  0000000141CD3710: shl         rcx,20h
  0000000141CD3714: or          rcx,r8
  0000000141CD3717: mov         r8d,1
  0000000141CD371D: mov         rax,rcx
  0000000141CD3720: mov         dword ptr [rbp+160h],r8d
  0000000141CD3727: mov         dword ptr [rbp+164h],edx
  0000000141CD372D: mov         qword ptr [rbp+168h],rax
  0000000141CD3734: lea         rax,[142F33670h]
  0000000141CD373B: mov         qword ptr [rsp+20h],rax
  0000000141CD3740: lea         rcx,[142F33400h]
  0000000141CD3747: lea         r9,[142F333E0h]
  0000000141CD374E: lea         r8,[rbp+160h]
  0000000141CD3755: mov         edx,2Bh
  0000000141CD375A: call        _ZN4core6result13unwrap_failed17h96d74ae09566f4b3E
  0000000141CD375F: ud2
  0000000141CD3761: nop         word ptr cs:[rax+rax]
  0000000141CD3770: mov         qword ptr [rsp+10h],rdx
  0000000141CD3775: push        rbp
  0000000141CD3776: push        r15
  0000000141CD3778: push        r14
  0000000141CD377A: push        r13
  0000000141CD377C: push        r12
  0000000141CD377E: push        rsi
  0000000141CD377F: push        rdi
  0000000141CD3780: push        rbx
  0000000141CD3781: sub         rsp,0D8h
  0000000141CD3788: lea         rbp,[rdx+80h]
  0000000141CD378F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3795: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD379C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD37A2: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD37A9: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD37AF: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD37B8: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD37C1: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD37CA: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD37D2: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD37DA: mov         rax,qword ptr [rbp+2B8h]
  0000000141CD37E1: inc         qword ptr [rax]
  0000000141CD37E4: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD37EC: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD37F4: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD37FD: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD3806: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD380F: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD3815: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CD381B: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3821: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CD3827: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD382D: add         rsp,0D8h
  0000000141CD3834: pop         rbx
  0000000141CD3835: pop         rdi
  0000000141CD3836: pop         rsi
  0000000141CD3837: pop         r12
  0000000141CD3839: pop         r13
  0000000141CD383B: pop         r14
  0000000141CD383D: pop         r15
  0000000141CD383F: pop         rbp
  0000000141CD3840: ret
  0000000141CD3841: nop         word ptr cs:[rax+rax]
  0000000141CD3850: mov         qword ptr [rsp+10h],rdx
  0000000141CD3855: push        rbp
  0000000141CD3856: push        r15
  0000000141CD3858: push        r14
  0000000141CD385A: push        r13
  0000000141CD385C: push        r12
  0000000141CD385E: push        rsi
  0000000141CD385F: push        rdi
  0000000141CD3860: push        rbx
  0000000141CD3861: sub         rsp,0D8h
  0000000141CD3868: lea         rbp,[rdx+80h]
  0000000141CD386F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3875: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD387C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD3882: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD3889: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD388F: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD3898: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD38A1: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD38AA: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD38B2: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD38BA: lea         rcx,[rbp+220h]
  0000000141CD38C1: call        _ZN4core3ptr100drop_in_place$LT$country_core..resources..stairs..stairs_removed_supports..StairsRemovedSupports$GT$17h6ac75686d43d1941E
  0000000141CD38C6: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD38CE: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD38D6: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD38DF: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD38E8: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD38F1: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD38F7: movdqa      xmm12,xmmword ptr [rsp+60h]
  0000000141CD38FE: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3904: movdqa      xmm14,xmmword ptr [rsp+40h]
  0000000141CD390B: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD3911: add         rsp,0D8h
  0000000141CD3918: pop         rbx
  0000000141CD3919: pop         rdi
  0000000141CD391A: pop         rsi
  0000000141CD391B: pop         r12
  0000000141CD391D: pop         r13
  0000000141CD391F: pop         r14
  0000000141CD3921: pop         r15
  0000000141CD3923: pop         rbp
  0000000141CD3924: ret
  0000000141CD3925: nop         word ptr cs:[rax+rax]
  0000000141CD3930: mov         qword ptr [rsp+10h],rdx
  0000000141CD3935: push        rbp
  0000000141CD3936: push        r15
  0000000141CD3938: push        r14
  0000000141CD393A: push        r13
  0000000141CD393C: push        r12
  0000000141CD393E: push        rsi
  0000000141CD393F: push        rdi
  0000000141CD3940: push        rbx
  0000000141CD3941: sub         rsp,0D8h
  0000000141CD3948: lea         rbp,[rdx+80h]
  0000000141CD394F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3955: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD395C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD3962: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD3969: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD396F: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD3978: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD3981: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD398A: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD3992: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD399A: cmp         qword ptr [rbp+210h],0
  0000000141CD39A2: je          0000000141CD39D3
  0000000141CD39A4: lea         rax,[rbp+218h]
  0000000141CD39AB: mov         qword ptr [rbp+120h],rax
  0000000141CD39B2: lea         rax,[rbp+120h]
  0000000141CD39B9: mov         qword ptr [rbp+118h],rax
  0000000141CD39C0: lea         rcx,[142F33490h]
  0000000141CD39C7: lea         rdx,[rbp+118h]
  0000000141CD39CE: call        _ZN3std6thread5local17LocalKey$LT$T$GT$4with17h467a4e8d8d5a85fdE
  0000000141CD39D3: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD39DB: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD39E3: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD39EC: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD39F5: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD39FE: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD3A04: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CD3A0A: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3A10: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CD3A16: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD3A1C: add         rsp,0D8h
  0000000141CD3A23: pop         rbx
  0000000141CD3A24: pop         rdi
  0000000141CD3A25: pop         rsi
  0000000141CD3A26: pop         r12
  0000000141CD3A28: pop         r13
  0000000141CD3A2A: pop         r14
  0000000141CD3A2C: pop         r15
  0000000141CD3A2E: pop         rbp
  0000000141CD3A2F: ret
  0000000141CD3A30: mov         qword ptr [rsp+10h],rdx
  0000000141CD3A35: push        rbp
  0000000141CD3A36: push        r15
  0000000141CD3A38: push        r14
  0000000141CD3A3A: push        r13
  0000000141CD3A3C: push        r12
  0000000141CD3A3E: push        rsi
  0000000141CD3A3F: push        rdi
  0000000141CD3A40: push        rbx
  0000000141CD3A41: sub         rsp,0D8h
  0000000141CD3A48: lea         rbp,[rdx+80h]
  0000000141CD3A4F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3A55: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD3A5C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD3A62: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD3A69: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD3A6F: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD3A78: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD3A81: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD3A8A: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD3A92: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD3A9A: lea         rcx,[rbp+80h]
  0000000141CD3AA1: call        _ZN4core3ptr100drop_in_place$LT$country_core..resources..stairs..stairs_removed_supports..StairsRemovedSupports$GT$17h6ac75686d43d1941E
  0000000141CD3AA6: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD3AAE: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD3AB6: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD3ABF: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD3AC8: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD3AD1: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD3AD7: movdqa      xmm12,xmmword ptr [rsp+60h]
  0000000141CD3ADE: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3AE4: movdqa      xmm14,xmmword ptr [rsp+40h]
  0000000141CD3AEB: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD3AF1: add         rsp,0D8h
  0000000141CD3AF8: pop         rbx
  0000000141CD3AF9: pop         rdi
  0000000141CD3AFA: pop         rsi
  0000000141CD3AFB: pop         r12
  0000000141CD3AFD: pop         r13
  0000000141CD3AFF: pop         r14
  0000000141CD3B01: pop         r15
  0000000141CD3B03: pop         rbp
  0000000141CD3B04: ret
  0000000141CD3B05: nop         word ptr cs:[rax+rax]
  0000000141CD3B10: mov         qword ptr [rsp+10h],rdx
  0000000141CD3B15: push        rbp
  0000000141CD3B16: push        r15
  0000000141CD3B18: push        r14
  0000000141CD3B1A: push        r13
  0000000141CD3B1C: push        r12
  0000000141CD3B1E: push        rsi
  0000000141CD3B1F: push        rdi
  0000000141CD3B20: push        rbx
  0000000141CD3B21: sub         rsp,0D8h
  0000000141CD3B28: lea         rbp,[rdx+80h]
  0000000141CD3B2F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3B35: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD3B3C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD3B42: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD3B49: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD3B4F: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD3B58: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD3B61: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD3B6A: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD3B72: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD3B7A: mov         rax,qword ptr [rbp+268h]
  0000000141CD3B81: test        rax,rax
  0000000141CD3B84: je          0000000141CD3BA0
  0000000141CD3B86: mov         rcx,qword ptr [rbp+270h]
  0000000141CD3B8D: shl         rax,2
  0000000141CD3B91: lea         rdx,[rax+rax*2]
  0000000141CD3B95: mov         r8d,4
  0000000141CD3B9B: call        __rust_dealloc
  0000000141CD3BA0: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD3BA8: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD3BB0: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD3BB9: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD3BC2: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD3BCB: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD3BD1: movdqa      xmm12,xmmword ptr [rsp+60h]
  0000000141CD3BD8: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3BDE: movdqa      xmm14,xmmword ptr [rsp+40h]
  0000000141CD3BE5: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD3BEB: add         rsp,0D8h
  0000000141CD3BF2: pop         rbx
  0000000141CD3BF3: pop         rdi
  0000000141CD3BF4: pop         rsi
  0000000141CD3BF5: pop         r12
  0000000141CD3BF7: pop         r13
  0000000141CD3BF9: pop         r14
  0000000141CD3BFB: pop         r15
  0000000141CD3BFD: pop         rbp
  0000000141CD3BFE: ret
  0000000141CD3BFF: nop
  0000000141CD3C00: mov         qword ptr [rsp+10h],rdx
  0000000141CD3C05: push        rbp
  0000000141CD3C06: push        r15
  0000000141CD3C08: push        r14
  0000000141CD3C0A: push        r13
  0000000141CD3C0C: push        r12
  0000000141CD3C0E: push        rsi
  0000000141CD3C0F: push        rdi
  0000000141CD3C10: push        rbx
  0000000141CD3C11: sub         rsp,0D8h
  0000000141CD3C18: lea         rbp,[rdx+80h]
  0000000141CD3C1F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CD3C25: movdqa      xmmword ptr [rsp+40h],xmm14
  0000000141CD3C2C: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CD3C32: movdqa      xmmword ptr [rsp+60h],xmm12
  0000000141CD3C39: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CD3C3F: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CD3C48: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CD3C51: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CD3C5A: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CD3C62: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CD3C6A: mov         rax,qword ptr [rbp+2D0h]
  0000000141CD3C71: inc         qword ptr [rax]
  0000000141CD3C74: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CD3C7C: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CD3C84: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CD3C8D: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CD3C96: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CD3C9F: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CD3CA5: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CD3CAB: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CD3CB1: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CD3CB7: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CD3CBD: add         rsp,0D8h
  0000000141CD3CC4: pop         rbx
  0000000141CD3CC5: pop         rdi
  0000000141CD3CC6: pop         rsi
  0000000141CD3CC7: pop         r12
  0000000141CD3CC9: pop         r13
  0000000141CD3CCB: pop         r14
  0000000141CD3CCD: pop         r15
  0000000141CD3CCF: pop         rbp
  0000000141CD3CD0: ret
  0000000141CD3CD1: CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC     ...............


_ZN14system_clutter20inter_shape_stitches26detect_intra_shape_corners30add_hole_at_shape_intersection17heb8e784f3b778c9cE:
  0000000141CD3CE0: push        r14
  0000000141CD3CE2: push        rsi
  0000000141CD3CE3: push        rdi
  0000000141CD3CE4: push        rbx
  0000000141CD3CE5: sub         rsp,0B8h
  0000000141CD3CEC: movaps      xmmword ptr [rsp+0A0h],xmm13
  0000000141CD3CF5: movaps      xmmword ptr [rsp+90h],xmm12
  0000000141CD3CFE: movaps      xmmword ptr [rsp+80h],xmm11
  0000000141CD3D07: movaps      xmmword ptr [rsp+70h],xmm10
  0000000141CD3D0D: movaps      xmmword ptr [rsp+60h],xmm9
  0000000141CD3D13: movaps      xmmword ptr [rsp+50h],xmm8
  0000000141CD3D19: movaps      xmmword ptr [rsp+40h],xmm7
  0000000141CD3D1E: movaps      xmmword ptr [rsp+30h],xmm6
  0000000141CD3D23: mov         rbx,r8
  0000000141CD3D26: mov         rdi,rdx
  0000000141CD3D29: mov         rsi,rcx
  0000000141CD3D2C: movss       xmm11,dword ptr [r9]
  0000000141CD3D31: movss       xmm10,dword ptr [r9+4]
  0000000141CD3D37: movaps      xmm8,xmm11
  0000000141CD3D3B: addss       xmm8,xmm10
  0000000141CD3D40: mulss       xmm8,dword ptr [__real@3f000000]
  0000000141CD3D49: lea         r14,[rsp+24h]
  0000000141CD3D4E: mov         rcx,r14
  0000000141CD3D51: mov         rdx,r8
  0000000141CD3D54: movaps      xmm2,xmm8
  0000000141CD3D58: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  0000000141CD3D5D: mov         rcx,r14
  0000000141CD3D60: mov         rdx,rdi
  0000000141CD3D63: call        _ZN5utils10wall_space14WallSpaceCoord16from_world_space17h3dd7969c0bf40780E
  0000000141CD3D68: movaps      xmm7,xmm0
  0000000141CD3D6B: movaps      xmm6,xmm1
  0000000141CD3D6E: add         rbx,8
  0000000141CD3D72: lea         r14,[rsp+24h]
  0000000141CD3D77: mov         rcx,r14
  0000000141CD3D7A: mov         rdx,rbx
  0000000141CD3D7D: movaps      xmm2,xmm8
  0000000141CD3D81: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  0000000141CD3D86: mov         rcx,r14
  0000000141CD3D89: mov         rdx,rdi
  0000000141CD3D8C: call        _ZN5utils10wall_space14WallSpaceCoord16from_world_space17h3dd7969c0bf40780E
  0000000141CD3D91: movaps      xmm9,xmm0
  0000000141CD3D95: movaps      xmm8,xmm1
  0000000141CD3D99: mov         rcx,rdi
  0000000141CD3D9C: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState10wall_space17h924d9f326fab1d39E
  0000000141CD3DA1: mov         dword ptr [rsp+24h],eax
  0000000141CD3DA5: movss       dword ptr [rsp+28h],xmm0
  0000000141CD3DAB: lea         rcx,[rsp+24h]
  0000000141CD3DB0: movaps      xmm1,xmm9
  0000000141CD3DB4: movaps      xmm2,xmm7
  0000000141CD3DB7: call        _ZN91_$LT$utils..wall_space..WallSpace$u20$as$u20$utils..comparative_space..ComparativeSpace$GT$8subtract17hf7a5d0707cc7372dE
  0000000141CD3DBC: movaps      xmm9,xmm0
  0000000141CD3DC0: subss       xmm8,xmm6
  0000000141CD3DC5: xorps       xmm12,xmm12
  0000000141CD3DC9: ucomiss     xmm12,xmm0
  0000000141CD3DCD: jbe         0000000141CD3DDE
  0000000141CD3DCF: movss       xmm0,dword ptr [rdi+30h]
  0000000141CD3DD4: addss       xmm9,xmm0
  0000000141CD3DD9: addss       xmm8,xmm0
  0000000141CD3DDE: movss       xmm13,dword ptr [__real@3f000000]
  0000000141CD3DE7: mulss       xmm13,xmm9
  0000000141CD3DEC: mov         rcx,rdi
  0000000141CD3DEF: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState10wall_space17h924d9f326fab1d39E
  0000000141CD3DF4: mov         dword ptr [rsp+24h],eax
  0000000141CD3DF8: movss       dword ptr [rsp+28h],xmm0
  0000000141CD3DFE: addss       xmm7,xmm13
  0000000141CD3E03: lea         rcx,[rsp+24h]
  0000000141CD3E08: movaps      xmm1,xmm7
  0000000141CD3E0B: call        _ZN91_$LT$utils..wall_space..WallSpace$u20$as$u20$utils..comparative_space..ComparativeSpace$GT$13project_clamp17h403dbe9f0dcfece2E
  0000000141CD3E10: mov         al,5
  0000000141CD3E12: ucomiss     xmm9,xmm12
  0000000141CD3E16: jbe         0000000141CD3E4A
  0000000141CD3E18: xorps       xmm1,xmm1
  0000000141CD3E1B: mulss       xmm8,xmm1
  0000000141CD3E20: mulss       xmm8,dword ptr [__real@3f000000]
  0000000141CD3E29: addss       xmm6,xmm8
  0000000141CD3E2E: subss       xmm10,xmm11
  0000000141CD3E33: movss       dword ptr [rsi],xmm0
  0000000141CD3E37: movss       dword ptr [rsi+4],xmm6
  0000000141CD3E3C: movss       dword ptr [rsi+8],xmm9
  0000000141CD3E42: movss       dword ptr [rsi+0Ch],xmm10
  0000000141CD3E48: mov         al,1
  0000000141CD3E4A: mov         byte ptr [rsi+10h],al
  0000000141CD3E4D: mov         rax,rsi
  0000000141CD3E50: movaps      xmm6,xmmword ptr [rsp+30h]
  0000000141CD3E55: movaps      xmm7,xmmword ptr [rsp+40h]
  0000000141CD3E5A: movaps      xmm8,xmmword ptr [rsp+50h]
  0000000141CD3E60: movaps      xmm9,xmmword ptr [rsp+60h]
  0000000141CD3E66: movaps      xmm10,xmmword ptr [rsp+70h]
  0000000141CD3E6C: movaps      xmm11,xmmword ptr [rsp+80h]
  0000000141CD3E75: movaps      xmm12,xmmword ptr [rsp+90h]
  0000000141CD3E7E: movaps      xmm13,xmmword ptr [rsp+0A0h]
  0000000141CD3E87: add         rsp,0B8h
  0000000141CD3E8E: pop         rbx
  0000000141CD3E8F: pop         rdi
  0000000141CD3E90: pop         rsi
  0000000141CD3E91: pop         r14
  0000000141CD3E93: ret
  0000000141CD3E94: CC CC CC CC CC CC CC CC CC CC CC CC              ............


_ZN14system_clutter20inter_shape_stitches13stitch_bricks14spawn_stitches17h90520b5f03d10550E:
  0000000141CE65B0: push        rbp
  0000000141CE65B1: push        r15
  0000000141CE65B3: push        r14
  0000000141CE65B5: push        r13
  0000000141CE65B7: push        r12
  0000000141CE65B9: push        rsi
  0000000141CE65BA: push        rdi
  0000000141CE65BB: push        rbx
  0000000141CE65BC: sub         rsp,198h
  0000000141CE65C3: lea         rbp,[rsp+80h]
  0000000141CE65CB: movaps      xmmword ptr [rbp+100h],xmm15
  0000000141CE65D3: movaps      xmmword ptr [rbp+0F0h],xmm14
  0000000141CE65DB: movaps      xmmword ptr [rbp+0E0h],xmm13
  0000000141CE65E3: movaps      xmmword ptr [rbp+0D0h],xmm12
  0000000141CE65EB: movaps      xmmword ptr [rbp+0C0h],xmm11
  0000000141CE65F3: movaps      xmmword ptr [rbp+0B0h],xmm10
  0000000141CE65FB: movaps      xmmword ptr [rbp+0A0h],xmm9
  0000000141CE6603: movaps      xmmword ptr [rbp+90h],xmm8
  0000000141CE660B: movaps      xmmword ptr [rbp+80h],xmm7
  0000000141CE6612: movaps      xmmword ptr [rbp+70h],xmm6
  0000000141CE6616: mov         qword ptr [rbp+68h],0FFFFFFFFFFFFFFFEh
  0000000141CE661E: mov         rbx,rdx
  0000000141CE6621: mov         rdi,rcx
  0000000141CE6624: call        _ZN6puffin13are_scopes_on17h8f511bd2e57f501aE
  0000000141CE6629: test        al,al
  0000000141CE662B: mov         byte ptr [rbp+60h],al
  0000000141CE662E: je          0000000141CE668F
  0000000141CE6630: lea         r14,[142F360F3h]
  0000000141CE6637: mov         esi,43h
  0000000141CE663C: mov         edx,43h
  0000000141CE6641: mov         rcx,r14
  0000000141CE6644: call        0000000141CE6190
  0000000141CE6649: cmp         rax,1
  0000000141CE664D: jne         0000000141CE6704
  0000000141CE6653: mov         r9,rdx
  0000000141CE6656: test        rdx,rdx
  0000000141CE6659: je          0000000141CE6698
  0000000141CE665B: cmp         r9,43h
  0000000141CE665F: jae         0000000141CE6696
  0000000141CE6661: lea         rax,[142F360F3h]
  0000000141CE6668: cmp         byte ptr [r9+rax],0BFh
  0000000141CE666D: jg          0000000141CE6698
  0000000141CE666F: lea         rax,[142F361C0h]
  0000000141CE6676: mov         qword ptr [rsp+20h],rax
  0000000141CE667B: lea         rcx,[142F360F3h]
  0000000141CE6682: mov         edx,43h
  0000000141CE6687: xor         r8d,r8d
  0000000141CE668A: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE668F: xor         eax,eax
  0000000141CE6691: jmp         0000000141CE685E
  0000000141CE6696: jne         0000000141CE666F
  0000000141CE6698: lea         r14,[142F360F3h]
  0000000141CE669F: mov         rcx,r14
  0000000141CE66A2: mov         rdx,r9
  0000000141CE66A5: call        0000000141CE6190
  0000000141CE66AA: cmp         rax,1
  0000000141CE66AE: jne         0000000141CE6704
  0000000141CE66B0: mov         r8,rdx
  0000000141CE66B3: add         r8,2
  0000000141CE66B7: je          0000000141CE66F2
  0000000141CE66B9: cmp         r8,43h
  0000000141CE66BD: jae         0000000141CE66F0
  0000000141CE66BF: lea         rax,[142F360F3h]
  0000000141CE66C6: cmp         byte ptr [r8+rax],0BFh
  0000000141CE66CB: jg          0000000141CE66F2
  0000000141CE66CD: lea         rax,[142F361D8h]
  0000000141CE66D4: mov         qword ptr [rsp+20h],rax
  0000000141CE66D9: lea         rcx,[142F360F3h]
  0000000141CE66E0: mov         edx,43h
  0000000141CE66E5: mov         r9d,43h
  0000000141CE66EB: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE66F0: jne         0000000141CE66CD
  0000000141CE66F2: mov         esi,41h
  0000000141CE66F7: sub         rsi,rdx
  0000000141CE66FA: lea         r14,[142F360F3h]
  0000000141CE6701: add         r14,r8
  0000000141CE6704: lea         r8,[142F36248h]
  0000000141CE670B: lea         r15,[142F36208h]
  0000000141CE6712: nop         word ptr cs:[rax+rax]
  0000000141CE6720: movsx       eax,byte ptr [r8-1]
  0000000141CE6725: test        eax,eax
  0000000141CE6727: js          0000000141CE6740
  0000000141CE6729: dec         r8
  0000000141CE672C: cmp         eax,5Ch
  0000000141CE672F: jne         0000000141CE6794
  0000000141CE6731: jmp         0000000141CE67A6
  0000000141CE6733: nop         word ptr cs:[rax+rax]
  0000000141CE6740: movzx       ecx,byte ptr [r8-2]
  0000000141CE6745: cmp         cl,0C0h
  0000000141CE6748: jge         0000000141CE676D
  0000000141CE674A: movzx       edx,byte ptr [r8-3]
  0000000141CE674F: cmp         dl,0C0h
  0000000141CE6752: jge         0000000141CE6776
  0000000141CE6754: movzx       r9d,byte ptr [r8-4]
  0000000141CE6759: add         r8,0FFFFFFFFFFFFFFFCh
  0000000141CE675D: and         r9d,7
  0000000141CE6761: shl         r9d,6
  0000000141CE6765: and         edx,3Fh
  0000000141CE6768: or          edx,r9d
  0000000141CE676B: jmp         0000000141CE677D
  0000000141CE676D: add         r8,0FFFFFFFFFFFFFFFEh
  0000000141CE6771: and         ecx,1Fh
  0000000141CE6774: jmp         0000000141CE6785
  0000000141CE6776: add         r8,0FFFFFFFFFFFFFFFDh
  0000000141CE677A: and         edx,0Fh
  0000000141CE677D: shl         edx,6
  0000000141CE6780: and         ecx,3Fh
  0000000141CE6783: or          ecx,edx
  0000000141CE6785: shl         ecx,6
  0000000141CE6788: and         al,3Fh
  0000000141CE678A: movzx       eax,al
  0000000141CE678D: or          eax,ecx
  0000000141CE678F: cmp         eax,5Ch
  0000000141CE6792: je          0000000141CE67A6
  0000000141CE6794: cmp         eax,2Fh
  0000000141CE6797: je          0000000141CE67A6
  0000000141CE6799: cmp         r8,r15
  0000000141CE679C: jne         0000000141CE6720
  0000000141CE679E: mov         r12d,40h
  0000000141CE67A4: jmp         0000000141CE67F3
  0000000141CE67A6: lea         r15,[142F36208h]
  0000000141CE67AD: sub         r8,r15
  0000000141CE67B0: inc         r8
  0000000141CE67B3: je          0000000141CE67E7
  0000000141CE67B5: cmp         r8,40h
  0000000141CE67B9: jae         0000000141CE67E5
  0000000141CE67BB: cmp         byte ptr [r8+r15],0BFh
  0000000141CE67C0: jg          0000000141CE67E7
  0000000141CE67C2: lea         rax,[142F361A0h]
  0000000141CE67C9: mov         qword ptr [rsp+20h],rax
  0000000141CE67CE: lea         rcx,[142F36208h]
  0000000141CE67D5: mov         edx,40h
  0000000141CE67DA: mov         r9d,40h
  0000000141CE67E0: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE67E5: jne         0000000141CE67C2
  0000000141CE67E7: mov         r12d,40h
  0000000141CE67ED: sub         r12,r8
  0000000141CE67F0: add         r15,r8
  0000000141CE67F3: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CE67F8: mov         rcx,rax
  0000000141CE67FB: mov         rax,qword ptr [rax]
  0000000141CE67FE: cmp         rax,1
  0000000141CE6802: jne         0000000141CE6DBC
  0000000141CE6808: add         rcx,8
  0000000141CE680C: cmp         qword ptr [rcx],0
  0000000141CE6810: jne         0000000141CE6DFA
  0000000141CE6816: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CE681D: mov         qword ptr [rbp+58h],rcx
  0000000141CE6821: add         rcx,8
  0000000141CE6825: mov         qword ptr [rsp+20h],r12
  0000000141CE682A: mov         qword ptr [rsp+30h],0
  0000000141CE6833: mov         qword ptr [rsp+28h],1
  0000000141CE683C: mov         rdx,r14
  0000000141CE683F: mov         r8,rsi
  0000000141CE6842: mov         r9,r15
  0000000141CE6845: call        _ZN6puffin14ThreadProfiler11begin_scope17h161a40482b7cfe19E
  0000000141CE684A: mov         rcx,qword ptr [rbp+58h]
  0000000141CE684E: inc         qword ptr [rcx]
  0000000141CE6851: mov         qword ptr [rbp+8],rax
  0000000141CE6855: mov         qword ptr [rbp-38h],rax
  0000000141CE6859: mov         eax,1
  0000000141CE685E: mov         qword ptr [rbp-40h],rax
  0000000141CE6862: mov         eax,dword ptr [rdi+1Ch]
  0000000141CE6865: mov         rcx,qword ptr [rdi+10h]
  0000000141CE6869: mov         dword ptr [rcx],eax
  0000000141CE686B: mov         rdi,qword ptr [rdi]
  0000000141CE686E: mov         rax,qword ptr [rbx]
  0000000141CE6871: mov         r13,qword ptr [rax+8]
  0000000141CE6875: mov         rbx,qword ptr [rax+10h]
  0000000141CE6879: shl         rbx,6
  0000000141CE687D: add         rbx,r13
  0000000141CE6880: xorps       xmm8,xmm8
  0000000141CE6884: movss       xmm9,dword ptr [__real@447a0000]
  0000000141CE688D: movss       xmm11,dword ptr [__real@5f7fffff]
  0000000141CE6896: movss       xmm14,dword ptr [__real@3f000000]
  0000000141CE689F: add         r13,28h
  0000000141CE68A3: jmp         0000000141CE691C
  0000000141CE68A5: nop         word ptr cs:[rax+rax]
  0000000141CE68B0: movss       xmm13,dword ptr [r13-14h]
  0000000141CE68B6: subss       xmm13,dword ptr [r13-18h]
  0000000141CE68BC: movaps      xmm0,xmm13
  0000000141CE68C0: addss       xmm0,xmm13
  0000000141CE68C5: xorps       xmm1,xmm1
  0000000141CE68C8: maxss       xmm1,xmm0
  0000000141CE68CC: movaps      xmm0,xmm9
  0000000141CE68D0: minss       xmm0,xmm1
  0000000141CE68D4: cvttss2si   rax,xmm0
  0000000141CE68D9: mov         rcx,rax
  0000000141CE68DC: sar         rcx,3Fh
  0000000141CE68E0: movaps      xmm1,xmm0
  0000000141CE68E3: subss       xmm1,dword ptr [__real@5f000000]
  0000000141CE68EB: cvttss2si   rsi,xmm1
  0000000141CE68F0: and         rsi,rcx
  0000000141CE68F3: or          rsi,rax
  0000000141CE68F6: ucomiss     xmm0,xmm8
  0000000141CE68FA: mov         eax,0
  0000000141CE68FF: cmovb       rsi,rax
  0000000141CE6903: ucomiss     xmm0,xmm11
  0000000141CE6907: mov         rax,0FFFFFFFFFFFFFFFFh
  0000000141CE690E: cmova       rsi,rax
  0000000141CE6912: cmp         rsi,2
  0000000141CE6916: ja          0000000141CE6950
  0000000141CE6918: add         r13,40h
  0000000141CE691C: lea         rax,[r13-28h]
  0000000141CE6920: cmp         rax,rbx
  0000000141CE6923: je          0000000141CE6D15
  0000000141CE6929: cmp         dword ptr [rax],1
  0000000141CE692C: jne         0000000141CE68B0
  0000000141CE692E: movss       xmm0,dword ptr [__real@3e4ccccd]
  0000000141CE6936: ucomiss     xmm0,dword ptr [r13-24h]
  0000000141CE693B: jbe         0000000141CE68B0
  0000000141CE6941: jmp         0000000141CE6918
  0000000141CE6943: nop         word ptr cs:[rax+rax]
  0000000141CE6950: mov         rcx,r13
  0000000141CE6953: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims9get_inner17hb0e1eb6e085c9aa5E
  0000000141CE6958: mov         r15,rax
  0000000141CE695B: lea         rcx,[r13+8]
  0000000141CE695F: call        _ZN12country_core9resources5roofs10roof_ridge13RoofRidgeDims9get_inner17hb0e1eb6e085c9aa5E
  0000000141CE6964: add         rax,r15
  0000000141CE6967: mov         qword ptr [rbp+28h],rax
  0000000141CE696B: dec         rsi
  0000000141CE696E: movss       xmm2,dword ptr [__real@3e4ccccd]
  0000000141CE6976: divss       xmm2,xmm13
  0000000141CE697B: lea         rax,[142F36248h]
  0000000141CE6982: mov         qword ptr [rsp+20h],rax
  0000000141CE6987: lea         rcx,[rbp-18h]
  0000000141CE698B: mov         rdx,rsi
  0000000141CE698E: lea         r9,[rbp+28h]
  0000000141CE6992: call        _ZN5utils13random_splits17h445a73628407a507E
  0000000141CE6997: movsd       xmm0,mmword ptr [r13-10h]
  0000000141CE699D: movsd       xmm1,mmword ptr [r13-8]
  0000000141CE69A3: addps       xmm1,xmm0
  0000000141CE69A6: movaps      xmm0,xmm1
  0000000141CE69A9: mulps       xmm0,xmm1
  0000000141CE69AC: movshdup    xmm2,xmm0
  0000000141CE69B0: addss       xmm2,xmm0
  0000000141CE69B4: xorps       xmm0,xmm0
  0000000141CE69B7: sqrtss      xmm0,xmm2
  0000000141CE69BB: movss       xmm2,dword ptr [__real@3f800000]
  0000000141CE69C3: divss       xmm2,xmm0
  0000000141CE69C7: movsldup    xmm0,xmm2
  0000000141CE69CB: mulps       xmm0,xmm1
  0000000141CE69CE: movlps      qword ptr [rbp-20h],xmm0
  0000000141CE69D2: movshdup    xmm1,xmm0
  0000000141CE69D6: xorps       xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000141CE69DD: movss       dword ptr [rbp+4Ch],xmm1
  0000000141CE69E2: movss       dword ptr [rbp+50h],xmm0
  0000000141CE69E7: lea         rcx,[rbp+30h]
  0000000141CE69EB: lea         rdx,[rbp+4Ch]
  0000000141CE69EF: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  0000000141CE69F4: movss       xmm10,dword ptr [rbp+30h]
  0000000141CE69FA: movss       xmm7,dword ptr [rbp+34h]
  0000000141CE69FF: movss       xmm15,dword ptr [rbp+38h]
  0000000141CE6A05: lea         rcx,[rbp+30h]
  0000000141CE6A09: lea         rdx,[rbp-20h]
  0000000141CE6A0D: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  0000000141CE6A12: movsd       xmm0,mmword ptr [rbp+30h]
  0000000141CE6A17: movss       xmm2,dword ptr [rbp+38h]
  0000000141CE6A1C: ucomiss     xmm8,xmm2
  0000000141CE6A20: jae         0000000141CE6A90
  0000000141CE6A22: xorps       xmm7,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000141CE6A29: movss       xmm1,dword ptr [__real@3f800000]
  0000000141CE6A31: movaps      xmm3,xmm1
  0000000141CE6A34: subss       xmm3,xmm10
  0000000141CE6A39: addss       xmm2,xmm1
  0000000141CE6A3D: ucomiss     xmm8,xmm3
  0000000141CE6A41: jae         0000000141CE6AF4
  0000000141CE6A47: addss       xmm3,xmm2
  0000000141CE6A4B: xorps       xmm2,xmm2
  0000000141CE6A4E: sqrtss      xmm2,xmm3
  0000000141CE6A52: movaps      xmm1,xmm14
  0000000141CE6A56: divss       xmm1,xmm2
  0000000141CE6A5A: xorps       xmm2,xmm2
  0000000141CE6A5D: subps       xmm2,xmm0
  0000000141CE6A60: addps       xmm15,xmm0
  0000000141CE6A64: shufps      xmm15,xmm2,10h
  0000000141CE6A69: shufps      xmm15,xmm2,0E2h
  0000000141CE6A6E: movsldup    xmm12,xmm1
  0000000141CE6A73: mulps       xmm12,xmm15
  0000000141CE6A77: mulss       xmm7,xmm1
  0000000141CE6A7B: mulss       xmm1,xmm3
  0000000141CE6A7F: jmp         0000000141CE6B70
  0000000141CE6A84: nop         word ptr cs:[rax+rax]
  0000000141CE6A90: movss       xmm12,dword ptr [__real@3f800000]
  0000000141CE6A99: addss       xmm10,xmm12
  0000000141CE6A9E: subss       xmm12,xmm2
  0000000141CE6AA3: ucomiss     xmm8,xmm10
  0000000141CE6AA7: jae         0000000141CE6B2E
  0000000141CE6AAD: addss       xmm10,xmm12
  0000000141CE6AB2: xorps       xmm1,xmm1
  0000000141CE6AB5: sqrtss      xmm1,xmm10
  0000000141CE6ABA: movaps      xmm12,xmm14
  0000000141CE6ABE: divss       xmm12,xmm1
  0000000141CE6AC3: xorps       xmm1,xmm1
  0000000141CE6AC6: subss       xmm1,xmm7
  0000000141CE6ACA: unpcklps    xmm1,xmm12
  0000000141CE6ACE: movshdup    xmm7,xmm0
  0000000141CE6AD2: addss       xmm7,xmm8
  0000000141CE6AD7: mulss       xmm7,xmm12
  0000000141CE6ADC: addss       xmm15,xmm0
  0000000141CE6AE1: mulss       xmm15,xmm12
  0000000141CE6AE6: unpcklps    xmm12,xmm10
  0000000141CE6AEA: mulps       xmm12,xmm1
  0000000141CE6AEE: movaps      xmm1,xmm15
  0000000141CE6AF2: jmp         0000000141CE6B70
  0000000141CE6AF4: subss       xmm2,xmm3
  0000000141CE6AF8: xorps       xmm3,xmm3
  0000000141CE6AFB: sqrtss      xmm3,xmm2
  0000000141CE6AFF: movaps      xmm1,xmm14
  0000000141CE6B03: divss       xmm1,xmm3
  0000000141CE6B07: movshdup    xmm3,xmm0
  0000000141CE6B0B: subss       xmm0,xmm15
  0000000141CE6B10: addss       xmm3,xmm8
  0000000141CE6B15: unpcklps    xmm0,xmm3
  0000000141CE6B18: movsldup    xmm12,xmm1
  0000000141CE6B1D: mulps       xmm12,xmm0
  0000000141CE6B21: mulss       xmm2,xmm1
  0000000141CE6B25: mulss       xmm1,xmm7
  0000000141CE6B29: movaps      xmm7,xmm2
  0000000141CE6B2C: jmp         0000000141CE6B70
  0000000141CE6B2E: subss       xmm12,xmm10
  0000000141CE6B33: xorps       xmm1,xmm1
  0000000141CE6B36: sqrtss      xmm1,xmm12
  0000000141CE6B3B: movaps      xmm2,xmm14
  0000000141CE6B3F: divss       xmm2,xmm1
  0000000141CE6B43: xorps       xmm3,xmm3
  0000000141CE6B46: subss       xmm3,xmm7
  0000000141CE6B4A: unpcklps    xmm12,xmm2
  0000000141CE6B4E: movshdup    xmm4,xmm0
  0000000141CE6B52: subss       xmm0,xmm15
  0000000141CE6B57: mulss       xmm0,xmm2
  0000000141CE6B5B: xorps       xmm1,xmm1
  0000000141CE6B5E: subss       xmm1,xmm4
  0000000141CE6B62: mulss       xmm1,xmm2
  0000000141CE6B66: unpcklps    xmm2,xmm3
  0000000141CE6B69: mulps       xmm12,xmm2
  0000000141CE6B6D: movaps      xmm7,xmm0
  0000000141CE6B70: movlhps     xmm1,xmm7
  0000000141CE6B73: shufps      xmm12,xmm1,24h
  0000000141CE6B78: lea         rax,[r13+18h]
  0000000141CE6B7C: mov         qword ptr [rbp+58h],rax
  0000000141CE6B80: mov         r12,qword ptr [rbp-10h]
  0000000141CE6B84: mov         rax,qword ptr [rbp-8]
  0000000141CE6B88: lea         r14,[r12+rax*4]
  0000000141CE6B8C: lea         rsi,[r13-20h]
  0000000141CE6B90: xor         eax,eax
  0000000141CE6B92: mov         qword ptr [rbp],r12
  0000000141CE6B96: jmp         0000000141CE6BF3
  0000000141CE6B98: nop         dword ptr [rax+rax]
  0000000141CE6BA0: mov         rax,qword ptr [rdi+8]
  0000000141CE6BA4: mov         rcx,r15
  0000000141CE6BA7: shl         rcx,6
  0000000141CE6BAB: movaps      xmmword ptr [rax+rcx],xmm12
  0000000141CE6BB0: mov         rdx,qword ptr [rbp+10h]
  0000000141CE6BB4: mov         qword ptr [rax+rcx+10h],rdx
  0000000141CE6BB9: mov         edx,dword ptr [rbp+18h]
  0000000141CE6BBC: mov         dword ptr [rax+rcx+18h],edx
  0000000141CE6BC0: movss       dword ptr [rax+rcx+1Ch],xmm6
  0000000141CE6BC6: movss       dword ptr [rax+rcx+20h],xmm15
  0000000141CE6BCD: mov         dword ptr [rax+rcx+24h],3E99999Ah
  0000000141CE6BD5: movups      xmm0,xmmword ptr [rbp+30h]
  0000000141CE6BD9: movups      xmmword ptr [rax+rcx+28h],xmm0
  0000000141CE6BDE: mov         rdx,qword ptr [rbp+40h]
  0000000141CE6BE2: mov         qword ptr [rax+rcx+38h],rdx
  0000000141CE6BE7: inc         r15
  0000000141CE6BEA: mov         qword ptr [rdi+10h],r15
  0000000141CE6BEE: mov         al,1
  0000000141CE6BF0: movaps      xmm6,xmm7
  0000000141CE6BF3: cmp         r12,r14
  0000000141CE6BF6: je          0000000141CE6CF0
  0000000141CE6BFC: lea         rcx,[r12+4]
  0000000141CE6C01: movss       xmm2,dword ptr [r12]
  0000000141CE6C07: mulss       xmm2,xmm13
  0000000141CE6C0C: movss       xmm7,dword ptr [r13-18h]
  0000000141CE6C12: addss       xmm2,xmm7
  0000000141CE6C16: test        al,1
  0000000141CE6C18: je          0000000141CE6C30
  0000000141CE6C1A: movaps      xmm7,xmm2
  0000000141CE6C1D: movaps      xmm2,xmm6
  0000000141CE6C20: mov         r12,rcx
  0000000141CE6C23: jmp         0000000141CE6C4D
  0000000141CE6C25: nop         word ptr cs:[rax+rax]
  0000000141CE6C30: cmp         rcx,r14
  0000000141CE6C33: je          0000000141CE6CF0
  0000000141CE6C39: movss       xmm0,dword ptr [r12+4]
  0000000141CE6C40: mulss       xmm0,xmm13
  0000000141CE6C45: add         r12,8
  0000000141CE6C49: addss       xmm7,xmm0
  0000000141CE6C4D: movaps      xmm15,xmm7
  0000000141CE6C51: subss       xmm15,xmm2
  0000000141CE6C56: addss       xmm15,dword ptr [__real@bdcccccd]
  0000000141CE6C5F: addss       xmm2,xmm7
  0000000141CE6C63: mulss       xmm2,xmm14
  0000000141CE6C68: lea         rcx,[rbp+4Ch]
  0000000141CE6C6C: mov         rdx,rsi
  0000000141CE6C6F: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  0000000141CE6C74: lea         rcx,[rbp+28h]
  0000000141CE6C78: call        _ZN8fastrand3Rng3f3217h1acd4fbfd60defe5E
  0000000141CE6C7D: movaps      xmm6,xmm0
  0000000141CE6C80: movss       xmm0,dword ptr [__real@3f800000]
  0000000141CE6C88: subss       xmm0,xmm6
  0000000141CE6C8C: mulss       xmm0,xmm14
  0000000141CE6C91: mulss       xmm6,xmm14
  0000000141CE6C96: subss       xmm6,xmm0
  0000000141CE6C9A: mulss       xmm6,dword ptr [__real@3dcccccd]
  0000000141CE6CA2: addss       xmm6,dword ptr [__real@3ecccccd]
  0000000141CE6CAA: mov         eax,dword ptr [rbp+54h]
  0000000141CE6CAD: mov         dword ptr [rbp+18h],eax
  0000000141CE6CB0: mov         rax,qword ptr [rbp+4Ch]
  0000000141CE6CB4: mov         qword ptr [rbp+10h],rax
  0000000141CE6CB8: movups      xmm0,xmmword ptr [r13]
  0000000141CE6CBD: lea         rax,[rbp+38h]
  0000000141CE6CC1: movups      xmmword ptr [rax],xmm0
  0000000141CE6CC4: mov         r15,qword ptr [rdi+10h]
  0000000141CE6CC8: cmp         r15,qword ptr [rdi]
  0000000141CE6CCB: jne         0000000141CE6BA0
  0000000141CE6CD1: mov         rcx,rdi
  0000000141CE6CD4: lea         rdx,[142F36260h]
  0000000141CE6CDB: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h47bc9420be9e6f61E
  0000000141CE6CE0: jmp         0000000141CE6BA0
  0000000141CE6CE5: nop         word ptr cs:[rax+rax]
  0000000141CE6CF0: mov         rdx,qword ptr [rbp-18h]
  0000000141CE6CF4: test        rdx,rdx
  0000000141CE6CF7: je          0000000141CE6D0C
  0000000141CE6CF9: shl         rdx,2
  0000000141CE6CFD: mov         r8d,4
  0000000141CE6D03: mov         rcx,qword ptr [rbp]
  0000000141CE6D07: call        __rust_dealloc
  0000000141CE6D0C: mov         r13,qword ptr [rbp+58h]
  0000000141CE6D10: jmp         0000000141CE689F
  0000000141CE6D15: cmp         byte ptr [rbp+60h],0
  0000000141CE6D19: je          0000000141CE6D5D
  0000000141CE6D1B: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CE6D20: mov         rcx,rax
  0000000141CE6D23: mov         rax,qword ptr [rax]
  0000000141CE6D26: cmp         rax,1
  0000000141CE6D2A: jne         0000000141CE6DD6
  0000000141CE6D30: add         rcx,8
  0000000141CE6D34: cmp         qword ptr [rcx],0
  0000000141CE6D38: jne         0000000141CE6DFA
  0000000141CE6D3E: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CE6D45: mov         qword ptr [rbp+60h],rcx
  0000000141CE6D49: add         rcx,8
  0000000141CE6D4D: mov         rdx,qword ptr [rbp+8]
  0000000141CE6D51: call        _ZN6puffin14ThreadProfiler9end_scope17hbdd5f34d320e5739E
  0000000141CE6D56: mov         rax,qword ptr [rbp+60h]
  0000000141CE6D5A: inc         qword ptr [rax]
  0000000141CE6D5D: movaps      xmm6,xmmword ptr [rbp+70h]
  0000000141CE6D61: movaps      xmm7,xmmword ptr [rbp+80h]
  0000000141CE6D68: movaps      xmm8,xmmword ptr [rbp+90h]
  0000000141CE6D70: movaps      xmm9,xmmword ptr [rbp+0A0h]
  0000000141CE6D78: movaps      xmm10,xmmword ptr [rbp+0B0h]
  0000000141CE6D80: movaps      xmm11,xmmword ptr [rbp+0C0h]
  0000000141CE6D88: movaps      xmm12,xmmword ptr [rbp+0D0h]
  0000000141CE6D90: movaps      xmm13,xmmword ptr [rbp+0E0h]
  0000000141CE6D98: movaps      xmm14,xmmword ptr [rbp+0F0h]
  0000000141CE6DA0: movaps      xmm15,xmmword ptr [rbp+100h]
  0000000141CE6DA8: add         rsp,198h
  0000000141CE6DAF: pop         rbx
  0000000141CE6DB0: pop         rdi
  0000000141CE6DB1: pop         rsi
  0000000141CE6DB2: pop         r12
  0000000141CE6DB4: pop         r13
  0000000141CE6DB6: pop         r14
  0000000141CE6DB8: pop         r15
  0000000141CE6DBA: pop         rbp
  0000000141CE6DBB: ret
  0000000141CE6DBC: test        rax,rax
  0000000141CE6DBF: jne         0000000141CE6DEE
  0000000141CE6DC1: xor         edx,edx
  0000000141CE6DC3: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CE6DC8: mov         rcx,rax
  0000000141CE6DCB: test        rax,rax
  0000000141CE6DCE: jne         0000000141CE680C
  0000000141CE6DD4: jmp         0000000141CE6DEE
  0000000141CE6DD6: test        rax,rax
  0000000141CE6DD9: jne         0000000141CE6DEE
  0000000141CE6DDB: xor         edx,edx
  0000000141CE6DDD: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CE6DE2: mov         rcx,rax
  0000000141CE6DE5: test        rax,rax
  0000000141CE6DE8: jne         0000000141CE6D34
  0000000141CE6DEE: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.1.llvm.13421880302868465498]
  0000000141CE6DF5: call        _ZN3std6thread5local18panic_access_error17hcff639665d708376E
  0000000141CE6DFA: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.3.llvm.13421880302868465498]
  0000000141CE6E01: call        _ZN4core4cell22panic_already_borrowed17h54fda569c7a6ddb1E
  0000000141CE6E06: int         3
  0000000141CE6E07: nop         word ptr [rax+rax]
  0000000141CE6E10: mov         qword ptr [rsp+10h],rdx
  0000000141CE6E15: push        rbp
  0000000141CE6E16: push        r15
  0000000141CE6E18: push        r14
  0000000141CE6E1A: push        r13
  0000000141CE6E1C: push        r12
  0000000141CE6E1E: push        rsi
  0000000141CE6E1F: push        rdi
  0000000141CE6E20: push        rbx
  0000000141CE6E21: sub         rsp,0D8h
  0000000141CE6E28: lea         rbp,[rdx+80h]
  0000000141CE6E2F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE6E35: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE6E3B: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE6E41: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE6E47: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE6E4D: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE6E56: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE6E5F: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE6E68: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE6E70: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE6E78: mov         rax,qword ptr [rbp+58h]
  0000000141CE6E7C: inc         qword ptr [rax]
  0000000141CE6E7F: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE6E87: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE6E8F: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE6E98: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE6EA1: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE6EAA: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE6EB0: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE6EB6: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE6EBC: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE6EC2: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE6EC8: add         rsp,0D8h
  0000000141CE6ECF: pop         rbx
  0000000141CE6ED0: pop         rdi
  0000000141CE6ED1: pop         rsi
  0000000141CE6ED2: pop         r12
  0000000141CE6ED4: pop         r13
  0000000141CE6ED6: pop         r14
  0000000141CE6ED8: pop         r15
  0000000141CE6EDA: pop         rbp
  0000000141CE6EDB: ret
  0000000141CE6EDC: nop         dword ptr [rax]
  0000000141CE6EE0: mov         qword ptr [rsp+10h],rdx
  0000000141CE6EE5: push        rbp
  0000000141CE6EE6: push        r15
  0000000141CE6EE8: push        r14
  0000000141CE6EEA: push        r13
  0000000141CE6EEC: push        r12
  0000000141CE6EEE: push        rsi
  0000000141CE6EEF: push        rdi
  0000000141CE6EF0: push        rbx
  0000000141CE6EF1: sub         rsp,0D8h
  0000000141CE6EF8: lea         rbp,[rdx+80h]
  0000000141CE6EFF: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE6F05: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE6F0B: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE6F11: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE6F17: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE6F1D: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE6F26: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE6F2F: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE6F38: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE6F40: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE6F48: cmp         byte ptr [rbp+60h],0
  0000000141CE6F4C: je          0000000141CE6F6E
  0000000141CE6F4E: lea         rax,[rbp-38h]
  0000000141CE6F52: mov         qword ptr [rbp-28h],rax
  0000000141CE6F56: lea         rax,[rbp-28h]
  0000000141CE6F5A: mov         qword ptr [rbp-30h],rax
  0000000141CE6F5E: lea         rcx,[142F36140h]
  0000000141CE6F65: lea         rdx,[rbp-30h]
  0000000141CE6F69: call        _ZN3std6thread5local17LocalKey$LT$T$GT$4with17h467a4e8d8d5a85fdE
  0000000141CE6F6E: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE6F76: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE6F7E: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE6F87: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE6F90: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE6F99: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE6F9F: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE6FA5: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE6FAB: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE6FB1: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE6FB7: add         rsp,0D8h
  0000000141CE6FBE: pop         rbx
  0000000141CE6FBF: pop         rdi
  0000000141CE6FC0: pop         rsi
  0000000141CE6FC1: pop         r12
  0000000141CE6FC3: pop         r13
  0000000141CE6FC5: pop         r14
  0000000141CE6FC7: pop         r15
  0000000141CE6FC9: pop         rbp
  0000000141CE6FCA: ret
  0000000141CE6FCB: nop         dword ptr [rax+rax]
  0000000141CE6FD0: mov         qword ptr [rsp+10h],rdx
  0000000141CE6FD5: push        rbp
  0000000141CE6FD6: push        r15
  0000000141CE6FD8: push        r14
  0000000141CE6FDA: push        r13
  0000000141CE6FDC: push        r12
  0000000141CE6FDE: push        rsi
  0000000141CE6FDF: push        rdi
  0000000141CE6FE0: push        rbx
  0000000141CE6FE1: sub         rsp,0D8h
  0000000141CE6FE8: lea         rbp,[rdx+80h]
  0000000141CE6FEF: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE6FF5: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE6FFB: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE7001: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE7007: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE700D: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE7016: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE701F: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE7028: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7030: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7038: mov         rax,qword ptr [rbp+60h]
  0000000141CE703C: inc         qword ptr [rax]
  0000000141CE703F: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE7047: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE704F: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE7058: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7061: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE706A: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7070: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE7076: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE707C: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7082: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE7088: add         rsp,0D8h
  0000000141CE708F: pop         rbx
  0000000141CE7090: pop         rdi
  0000000141CE7091: pop         rsi
  0000000141CE7092: pop         r12
  0000000141CE7094: pop         r13
  0000000141CE7096: pop         r14
  0000000141CE7098: pop         r15
  0000000141CE709A: pop         rbp
  0000000141CE709B: ret
  0000000141CE709C: nop         dword ptr [rax]
  0000000141CE70A0: mov         qword ptr [rsp+10h],rdx
  0000000141CE70A5: push        rbp
  0000000141CE70A6: push        r15
  0000000141CE70A8: push        r14
  0000000141CE70AA: push        r13
  0000000141CE70AC: push        r12
  0000000141CE70AE: push        rsi
  0000000141CE70AF: push        rdi
  0000000141CE70B0: push        rbx
  0000000141CE70B1: sub         rsp,0D8h
  0000000141CE70B8: lea         rbp,[rdx+80h]
  0000000141CE70BF: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE70C5: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE70CB: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE70D1: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE70D7: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE70DD: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE70E6: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE70EF: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE70F8: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7100: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7108: mov         rdx,qword ptr [rbp-18h]
  0000000141CE710C: test        rdx,rdx
  0000000141CE710F: je          0000000141CE7124
  0000000141CE7111: mov         rcx,qword ptr [rbp-10h]
  0000000141CE7115: shl         rdx,2
  0000000141CE7119: mov         r8d,4
  0000000141CE711F: call        __rust_dealloc
  0000000141CE7124: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE712C: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE7134: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE713D: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7146: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE714F: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7155: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE715B: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE7161: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7167: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE716D: add         rsp,0D8h
  0000000141CE7174: pop         rbx
  0000000141CE7175: pop         rdi
  0000000141CE7176: pop         rsi
  0000000141CE7177: pop         r12
  0000000141CE7179: pop         r13
  0000000141CE717B: pop         r14
  0000000141CE717D: pop         r15
  0000000141CE717F: pop         rbp
  0000000141CE7180: ret
  0000000141CE7181: CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC     ...............


_ZN14system_clutter20inter_shape_stitches13stitch_bricks19spawn_stitch_bricks17h88572398a689b952E:
  0000000141CE7190: push        rbp
  0000000141CE7191: push        r15
  0000000141CE7193: push        r14
  0000000141CE7195: push        r13
  0000000141CE7197: push        r12
  0000000141CE7199: push        rsi
  0000000141CE719A: push        rdi
  0000000141CE719B: push        rbx
  0000000141CE719C: sub         rsp,248h
  0000000141CE71A3: lea         rbp,[rsp+80h]
  0000000141CE71AB: movaps      xmmword ptr [rbp+1B0h],xmm15
  0000000141CE71B3: movaps      xmmword ptr [rbp+1A0h],xmm14
  0000000141CE71BB: movaps      xmmword ptr [rbp+190h],xmm13
  0000000141CE71C3: movaps      xmmword ptr [rbp+180h],xmm12
  0000000141CE71CB: movaps      xmmword ptr [rbp+170h],xmm11
  0000000141CE71D3: movaps      xmmword ptr [rbp+160h],xmm10
  0000000141CE71DB: movaps      xmmword ptr [rbp+150h],xmm9
  0000000141CE71E3: movaps      xmmword ptr [rbp+140h],xmm8
  0000000141CE71EB: movaps      xmmword ptr [rbp+130h],xmm7
  0000000141CE71F2: movaps      xmmword ptr [rbp+120h],xmm6
  0000000141CE71F9: mov         qword ptr [rbp+118h],0FFFFFFFFFFFFFFFEh
  0000000141CE7204: mov         rdi,r8
  0000000141CE7207: mov         rbx,rdx
  0000000141CE720A: mov         rsi,rcx
  0000000141CE720D: call        _ZN6puffin13are_scopes_on17h8f511bd2e57f501aE
  0000000141CE7212: test        al,al
  0000000141CE7214: je          0000000141CE7276
  0000000141CE7216: lea         r15,[142F360A8h]
  0000000141CE721D: mov         r14d,48h
  0000000141CE7223: mov         edx,48h
  0000000141CE7228: mov         rcx,r15
  0000000141CE722B: call        0000000141CE6190
  0000000141CE7230: cmp         rax,1
  0000000141CE7234: jne         0000000141CE72EC
  0000000141CE723A: mov         r9,rdx
  0000000141CE723D: test        rdx,rdx
  0000000141CE7240: je          0000000141CE727F
  0000000141CE7242: cmp         r9,48h
  0000000141CE7246: jae         0000000141CE727D
  0000000141CE7248: lea         rax,[142F360A8h]
  0000000141CE724F: cmp         byte ptr [r9+rax],0BFh
  0000000141CE7254: jg          0000000141CE727F
  0000000141CE7256: lea         rax,[142F361C0h]
  0000000141CE725D: mov         qword ptr [rsp+20h],rax
  0000000141CE7262: lea         rcx,[142F360A8h]
  0000000141CE7269: mov         edx,48h
  0000000141CE726E: xor         r8d,r8d
  0000000141CE7271: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE7276: xor         eax,eax
  0000000141CE7278: jmp         0000000141CE7443
  0000000141CE727D: jne         0000000141CE7256
  0000000141CE727F: lea         r15,[142F360A8h]
  0000000141CE7286: mov         rcx,r15
  0000000141CE7289: mov         rdx,r9
  0000000141CE728C: call        0000000141CE6190
  0000000141CE7291: cmp         rax,1
  0000000141CE7295: jne         0000000141CE72EC
  0000000141CE7297: mov         r8,rdx
  0000000141CE729A: add         r8,2
  0000000141CE729E: je          0000000141CE72D9
  0000000141CE72A0: cmp         r8,48h
  0000000141CE72A4: jae         0000000141CE72D7
  0000000141CE72A6: lea         rax,[142F360A8h]
  0000000141CE72AD: cmp         byte ptr [r8+rax],0BFh
  0000000141CE72B2: jg          0000000141CE72D9
  0000000141CE72B4: lea         rax,[142F361D8h]
  0000000141CE72BB: mov         qword ptr [rsp+20h],rax
  0000000141CE72C0: lea         rcx,[142F360A8h]
  0000000141CE72C7: mov         edx,48h
  0000000141CE72CC: mov         r9d,48h
  0000000141CE72D2: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE72D7: jne         0000000141CE72B4
  0000000141CE72D9: mov         r14d,46h
  0000000141CE72DF: sub         r14,rdx
  0000000141CE72E2: lea         r15,[142F360A8h]
  0000000141CE72E9: add         r15,r8
  0000000141CE72EC: lea         r8,[142F36248h]
  0000000141CE72F3: lea         r12,[142F36208h]
  0000000141CE72FA: nop         word ptr [rax+rax]
  0000000141CE7300: movsx       eax,byte ptr [r8-1]
  0000000141CE7305: test        eax,eax
  0000000141CE7307: js          0000000141CE7320
  0000000141CE7309: dec         r8
  0000000141CE730C: cmp         eax,5Ch
  0000000141CE730F: jne         0000000141CE7374
  0000000141CE7311: jmp         0000000141CE7386
  0000000141CE7313: nop         word ptr cs:[rax+rax]
  0000000141CE7320: movzx       ecx,byte ptr [r8-2]
  0000000141CE7325: cmp         cl,0C0h
  0000000141CE7328: jge         0000000141CE734D
  0000000141CE732A: movzx       edx,byte ptr [r8-3]
  0000000141CE732F: cmp         dl,0C0h
  0000000141CE7332: jge         0000000141CE7356
  0000000141CE7334: movzx       r9d,byte ptr [r8-4]
  0000000141CE7339: add         r8,0FFFFFFFFFFFFFFFCh
  0000000141CE733D: and         r9d,7
  0000000141CE7341: shl         r9d,6
  0000000141CE7345: and         edx,3Fh
  0000000141CE7348: or          edx,r9d
  0000000141CE734B: jmp         0000000141CE735D
  0000000141CE734D: add         r8,0FFFFFFFFFFFFFFFEh
  0000000141CE7351: and         ecx,1Fh
  0000000141CE7354: jmp         0000000141CE7365
  0000000141CE7356: add         r8,0FFFFFFFFFFFFFFFDh
  0000000141CE735A: and         edx,0Fh
  0000000141CE735D: shl         edx,6
  0000000141CE7360: and         ecx,3Fh
  0000000141CE7363: or          ecx,edx
  0000000141CE7365: shl         ecx,6
  0000000141CE7368: and         al,3Fh
  0000000141CE736A: movzx       eax,al
  0000000141CE736D: or          eax,ecx
  0000000141CE736F: cmp         eax,5Ch
  0000000141CE7372: je          0000000141CE7386
  0000000141CE7374: cmp         eax,2Fh
  0000000141CE7377: je          0000000141CE7386
  0000000141CE7379: cmp         r8,r12
  0000000141CE737C: jne         0000000141CE7300
  0000000141CE737E: mov         r13d,40h
  0000000141CE7384: jmp         0000000141CE73D3
  0000000141CE7386: lea         r12,[142F36208h]
  0000000141CE738D: sub         r8,r12
  0000000141CE7390: inc         r8
  0000000141CE7393: je          0000000141CE73C7
  0000000141CE7395: cmp         r8,40h
  0000000141CE7399: jae         0000000141CE73C5
  0000000141CE739B: cmp         byte ptr [r8+r12],0BFh
  0000000141CE73A0: jg          0000000141CE73C7
  0000000141CE73A2: lea         rax,[142F361A0h]
  0000000141CE73A9: mov         qword ptr [rsp+20h],rax
  0000000141CE73AE: lea         rcx,[142F36208h]
  0000000141CE73B5: mov         edx,40h
  0000000141CE73BA: mov         r9d,40h
  0000000141CE73C0: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  0000000141CE73C5: jne         0000000141CE73A2
  0000000141CE73C7: mov         r13d,40h
  0000000141CE73CD: sub         r13,r8
  0000000141CE73D0: add         r12,r8
  0000000141CE73D3: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CE73D8: mov         rcx,rax
  0000000141CE73DB: mov         rax,qword ptr [rax]
  0000000141CE73DE: cmp         rax,1
  0000000141CE73E2: jne         0000000141CE78E2
  0000000141CE73E8: add         rcx,8
  0000000141CE73EC: cmp         qword ptr [rcx],0
  0000000141CE73F0: jne         0000000141CE791E
  0000000141CE73F6: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CE73FD: mov         qword ptr [rbp+110h],rcx
  0000000141CE7404: add         rcx,8
  0000000141CE7408: mov         qword ptr [rsp+20h],r13
  0000000141CE740D: mov         qword ptr [rsp+30h],0
  0000000141CE7416: mov         qword ptr [rsp+28h],1
  0000000141CE741F: mov         rdx,r15
  0000000141CE7422: mov         r8,r14
  0000000141CE7425: mov         r9,r12
  0000000141CE7428: call        _ZN6puffin14ThreadProfiler11begin_scope17h161a40482b7cfe19E
  0000000141CE742D: mov         rcx,qword ptr [rbp+110h]
  0000000141CE7434: inc         qword ptr [rcx]
  0000000141CE7437: mov         qword ptr [rbp+108h],rax
  0000000141CE743E: mov         eax,1
  0000000141CE7443: mov         qword ptr [rbp+100h],rax
  0000000141CE744A: mov         rcx,qword ptr [rbx]
  0000000141CE744D: lea         rdx,[anon.78d3d481e5a5afc01d058fe84fbba446.0.llvm.11526520697743150770]
  0000000141CE7454: call        _ZN6frozen26FrozenMap$LT$K$C$V$C$S$GT$3get17h0166953e75cee77aE
  0000000141CE7459: mov         rbx,rax
  0000000141CE745C: test        rax,rax
  0000000141CE745F: je          0000000141CE797A
  0000000141CE7465: lea         rax,[anon.584c1c5eecec72721120725a2c49f1cb.1.llvm.16787586538867573953]
  0000000141CE746C: mov         qword ptr [rbp+98h],rax
  0000000141CE7473: mov         qword ptr [rbp+0A0h],3Ah
  0000000141CE747E: mov         r14,rbx
  0000000141CE7481: add         r14,10h
  0000000141CE7485: cmp         qword ptr [rbx+18h],3Ah
  0000000141CE748A: jne         0000000141CE792A
  0000000141CE7490: mov         rdx,qword ptr [r14]
  0000000141CE7493: lea         rcx,[anon.584c1c5eecec72721120725a2c49f1cb.1.llvm.16787586538867573953]
  0000000141CE749A: mov         r8d,3Ah
  0000000141CE74A0: call        memcmp
  0000000141CE74A5: test        eax,eax
  0000000141CE74A7: jne         0000000141CE792A
  0000000141CE74AD: mov         rcx,qword ptr [rbx]
  0000000141CE74B0: mov         rax,qword ptr [rbx+8]
  0000000141CE74B4: call        qword ptr [rax+18h]
  0000000141CE74B7: mov         rbx,rax
  0000000141CE74BA: test        rax,rax
  0000000141CE74BD: je          0000000141CE797A
  0000000141CE74C3: mov         eax,dword ptr [rdi+1Ch]
  0000000141CE74C6: mov         rcx,qword ptr [rdi+10h]
  0000000141CE74CA: mov         dword ptr [rcx],eax
  0000000141CE74CC: mov         r14,qword ptr [rdi]
  0000000141CE74CF: cmp         qword ptr [r14],0
  0000000141CE74D3: je          0000000141CE783D
  0000000141CE74D9: mov         rdi,qword ptr [r14+8]
  0000000141CE74DD: test        rdi,rdi
  0000000141CE74E0: je          0000000141CE78B7
  0000000141CE74E6: lea         rcx,[rdi+18h]
  0000000141CE74EA: mov         dl,1
  0000000141CE74EC: xor         eax,eax
  0000000141CE74EE: lock cmpxchg byte ptr [rdi+18h],dl
  0000000141CE74F3: jne         0000000141CE7955
  0000000141CE74F9: call        _ZN16parking_lot_core11parking_lot13deadlock_impl16acquire_resource17hb2f16ac28e483830E
  0000000141CE74FE: mov         rax,qword ptr [rdi+10h]
  0000000141CE7502: test        rax,rax
  0000000141CE7505: sete        cl
  0000000141CE7508: shl         rax,2
  0000000141CE750C: add         rax,qword ptr [rdi+8]
  0000000141CE7510: add         rax,0FFFFFFFFFFFFFFFCh
  0000000141CE7514: sete        dl
  0000000141CE7517: or          dl,cl
  0000000141CE7519: je          0000000141CE752D
  0000000141CE751B: mov         rax,qword ptr [rbx+28h]
  0000000141CE751F: test        rax,rax
  0000000141CE7522: je          0000000141CE7988
  0000000141CE7528: lea         ecx,[rax-1]
  0000000141CE752B: jmp         0000000141CE7533
  0000000141CE752D: mov         ecx,dword ptr [rax]
  0000000141CE752F: mov         rax,qword ptr [rbx+28h]
  0000000141CE7533: mov         rdx,qword ptr [rbx+20h]
  0000000141CE7537: movups      xmm0,xmmword ptr [rbx+30h]
  0000000141CE753B: movups      xmm1,xmmword ptr [rbx+40h]
  0000000141CE753F: movups      xmm2,xmmword ptr [rbx+50h]
  0000000141CE7543: mov         dword ptr [rbp+0E8h],ecx
  0000000141CE7549: mov         qword ptr [rbp+98h],rdx
  0000000141CE7550: mov         qword ptr [rbp+0A0h],rax
  0000000141CE7557: movups      xmmword ptr [rbp+0A8h],xmm0
  0000000141CE755E: mov         qword ptr [rbp+0B8h],rdi
  0000000141CE7565: movups      xmmword ptr [rbp+0C0h],xmm1
  0000000141CE756C: mov         qword ptr [rbp+0D0h],rbx
  0000000141CE7573: movups      xmmword ptr [rbp+0D8h],xmm2
  0000000141CE757A: lea         rcx,[rbp+98h]
  0000000141CE7581: call        _ZN12country_core6render22sparse_instance_buffer35SparseInstanceBufferWriter$LT$T$GT$5clear17h13bcd8703c6d1dbbE
  0000000141CE7586: mov         rax,qword ptr [rsi]
  0000000141CE7589: mov         r12,qword ptr [rax+8]
  0000000141CE758D: mov         r13,qword ptr [rax+10h]
  0000000141CE7591: shl         r13,6
  0000000141CE7595: add         r13,r12
  0000000141CE7598: xor         esi,esi
  0000000141CE759A: movss       xmm6,dword ptr [__real@3f800000]
  0000000141CE75A2: lea         rdi,[rbp-10h]
  0000000141CE75A6: xorps       xmm7,xmm7
  0000000141CE75A9: movsd       xmm8,mmword ptr [__xmm@0000000000000000c2c80000c2c80000]
  0000000141CE75B2: movsd       xmm9,mmword ptr [__xmm@00000000000000003dcccccdbdcccccd]
  0000000141CE75BB: lea         rbx,[rbp+98h]
  0000000141CE75C2: lea         r14,[rbp+30h]
  0000000141CE75C6: cmp         r12,r13
  0000000141CE75C9: je          0000000141CE7765
  0000000141CE75CF: nop
  0000000141CE75D0: movups      xmm1,xmmword ptr [r12+1Ch]
  0000000141CE75D6: movups      xmm0,xmmword ptr [r12+20h]
  0000000141CE75DC: movaps      xmm11,xmmword ptr [r12]
  0000000141CE75E1: movaps      xmm2,xmmword ptr [r12+10h]
  0000000141CE75E7: movss       xmm4,dword ptr [r12+8]
  0000000141CE75EE: movaps      xmm13,xmm4
  0000000141CE75F2: addss       xmm13,xmm4
  0000000141CE75F7: mulss       xmm4,xmm13
  0000000141CE75FC: movaps      xmm5,xmm11
  0000000141CE7600: addps       xmm5,xmm11
  0000000141CE7604: movshdup    xmm3,xmm5
  0000000141CE7608: mulss       xmm3,xmm11
  0000000141CE760D: movaps      xmm10,xmm11
  0000000141CE7611: movsldup    xmm12,xmm13
  0000000141CE7616: mulps       xmm12,xmm11
  0000000141CE761A: shufps      xmm11,xmm11,0FFh
  0000000141CE761F: mulps       xmm10,xmm5
  0000000141CE7623: mulps       xmm5,xmm11
  0000000141CE7627: movaps      xmm14,xmm11
  0000000141CE762B: mulss       xmm14,xmm13
  0000000141CE7630: shufps      xmm5,xmm5,0E1h
  0000000141CE7634: movshdup    xmm15,xmm10
  0000000141CE7639: movaps      xmm11,xmm15
  0000000141CE763D: addss       xmm11,xmm4
  0000000141CE7642: movaps      xmm13,xmm6
  0000000141CE7646: subss       xmm13,xmm11
  0000000141CE764B: movaps      xmm11,xmm3
  0000000141CE764F: addss       xmm11,xmm14
  0000000141CE7654: unpcklps    xmm13,xmm11
  0000000141CE7658: movaps      xmm11,xmm12
  0000000141CE765C: subps       xmm11,xmm5
  0000000141CE7660: shufps      xmm13,xmm11,4
  0000000141CE7665: subss       xmm3,xmm14
  0000000141CE766A: addss       xmm4,xmm10
  0000000141CE766F: movaps      xmm14,xmm6
  0000000141CE7673: subss       xmm14,xmm4
  0000000141CE7678: unpcklps    xmm3,xmm14
  0000000141CE767C: addps       xmm5,xmm12
  0000000141CE7680: shufps      xmm3,xmm5,54h
  0000000141CE7684: shufps      xmm11,xmm5,1
  0000000141CE7689: addss       xmm10,xmm15
  0000000141CE768E: movaps      xmm4,xmm6
  0000000141CE7691: subss       xmm4,xmm10
  0000000141CE7696: shufps      xmm11,xmm4,2
  0000000141CE769B: shufps      xmm1,xmm1,0
  0000000141CE769F: mulps       xmm1,xmm13
  0000000141CE76A3: movaps      xmm4,xmm0
  0000000141CE76A6: shufps      xmm4,xmm0,0
  0000000141CE76AA: mulps       xmm4,xmm3
  0000000141CE76AD: shufps      xmm0,xmm0,55h
  0000000141CE76B1: mulps       xmm0,xmm11
  0000000141CE76B5: shufps      xmm2,xmm2,0A4h
  0000000141CE76B9: movaps      xmmword ptr [rbp-10h],xmm1
  0000000141CE76BD: movaps      xmmword ptr [rbp],xmm4
  0000000141CE76C1: movaps      xmmword ptr [rbp+10h],xmm0
  0000000141CE76C5: movaps      xmmword ptr [rbp+20h],xmm2
  0000000141CE76C9: lea         rcx,[rbp-40h]
  0000000141CE76CD: mov         rdx,rdi
  0000000141CE76D0: call        _ZN123_$LT$country_core..resources..render..Affine3Packed$u20$as$u20$core..convert..From$LT$glam..f32..affine3a..Affine3A$GT$$GT$4from17hc13e0976a765162aE
  0000000141CE76D5: movups      xmm0,xmmword ptr [r12+30h]
  0000000141CE76DB: movaps      xmmword ptr [rbp-10h],xmm0
  0000000141CE76DF: mov         qword ptr [rbp],rsi
  0000000141CE76E3: mov         rcx,rdi
  0000000141CE76E6: call        _ZN5utils14calculate_hash17h7926a0391d7768aeE
  0000000141CE76EB: mov         r15,rax
  0000000141CE76EE: lea         rax,[r12+30h]
  0000000141CE76F3: mov         ecx,esi
  0000000141CE76F5: and         ecx,1
  0000000141CE76F8: mov         rcx,qword ptr [rax+rcx*8]
  0000000141CE76FC: call        _ZN139_$LT$country_core..geometry..instanced_wall..BrickSourceId$u20$as$u20$core..convert..From$LT$country_core..resources..walls..WallId$GT$$GT$4from17h6ceda8a80dd01f01E
  0000000141CE7701: movups      xmm0,xmmword ptr [rbp-40h]
  0000000141CE7705: movups      xmm1,xmmword ptr [rbp-30h]
  0000000141CE7709: movups      xmm2,xmmword ptr [rbp-20h]
  0000000141CE770D: movaps      xmmword ptr [rbp+50h],xmm2
  0000000141CE7711: movaps      xmmword ptr [rbp+40h],xmm1
  0000000141CE7715: movaps      xmmword ptr [rbp+30h],xmm0
  0000000141CE7719: lea         rcx,[rbp+60h]
  0000000141CE771D: movups      xmmword ptr [rcx],xmm7
  0000000141CE7720: movsd       mmword ptr [rbp+70h],xmm8
  0000000141CE7726: mov         dword ptr [rbp+78h],0C2C80000h
  0000000141CE772D: mov         dword ptr [rbp+7Ch],r15d
  0000000141CE7731: mov         dword ptr [rbp+80h],eax
  0000000141CE7737: movsd       mmword ptr [rbp+84h],xmm9
  0000000141CE7740: mov         dword ptr [rbp+8Ch],4
  0000000141CE774A: mov         rcx,rbx
  0000000141CE774D: mov         rdx,r14
  0000000141CE7750: call        _ZN12country_core6render22sparse_instance_buffer35SparseInstanceBufferWriter$LT$T$GT$4push17h3c80ae6cfd1abbc7E
  0000000141CE7755: add         r12,40h
  0000000141CE7759: inc         rsi
  0000000141CE775C: cmp         r12,r13
  0000000141CE775F: jne         0000000141CE75D0
  0000000141CE7765: mov         rsi,qword ptr [rbp+0B8h]
  0000000141CE776C: add         rsi,18h
  0000000141CE7770: mov         rcx,rsi
  0000000141CE7773: call        _ZN16parking_lot_core11parking_lot13deadlock_impl16release_resource17h1c3ab4db915ad6f2E
  0000000141CE7778: xor         ecx,ecx
  0000000141CE777A: mov         al,1
  0000000141CE777C: lock cmpxchg byte ptr [rsi],cl
  0000000141CE7780: jne         0000000141CE796B
  0000000141CE7786: cmp         qword ptr [rbp+100h],0
  0000000141CE778E: je          0000000141CE77DB
  0000000141CE7790: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  0000000141CE7795: mov         rcx,rax
  0000000141CE7798: mov         rax,qword ptr [rax]
  0000000141CE779B: cmp         rax,1
  0000000141CE779F: jne         0000000141CE78C8
  0000000141CE77A5: add         rcx,8
  0000000141CE77A9: cmp         qword ptr [rcx],0
  0000000141CE77AD: jne         0000000141CE791E
  0000000141CE77B3: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  0000000141CE77BA: mov         qword ptr [rbp+110h],rcx
  0000000141CE77C1: add         rcx,8
  0000000141CE77C5: mov         rdx,qword ptr [rbp+108h]
  0000000141CE77CC: call        _ZN6puffin14ThreadProfiler9end_scope17hbdd5f34d320e5739E
  0000000141CE77D1: mov         rax,qword ptr [rbp+110h]
  0000000141CE77D8: inc         qword ptr [rax]
  0000000141CE77DB: movaps      xmm6,xmmword ptr [rbp+120h]
  0000000141CE77E2: movaps      xmm7,xmmword ptr [rbp+130h]
  0000000141CE77E9: movaps      xmm8,xmmword ptr [rbp+140h]
  0000000141CE77F1: movaps      xmm9,xmmword ptr [rbp+150h]
  0000000141CE77F9: movaps      xmm10,xmmword ptr [rbp+160h]
  0000000141CE7801: movaps      xmm11,xmmword ptr [rbp+170h]
  0000000141CE7809: movaps      xmm12,xmmword ptr [rbp+180h]
  0000000141CE7811: movaps      xmm13,xmmword ptr [rbp+190h]
  0000000141CE7819: movaps      xmm14,xmmword ptr [rbp+1A0h]
  0000000141CE7821: movaps      xmm15,xmmword ptr [rbp+1B0h]
  0000000141CE7829: add         rsp,248h
  0000000141CE7830: pop         rbx
  0000000141CE7831: pop         rdi
  0000000141CE7832: pop         rsi
  0000000141CE7833: pop         r12
  0000000141CE7835: pop         r13
  0000000141CE7837: pop         r14
  0000000141CE7839: pop         r15
  0000000141CE783B: pop         rbp
  0000000141CE783C: ret
  0000000141CE783D: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  0000000141CE7844: mov         ecx,20h
  0000000141CE7849: mov         edx,8
  0000000141CE784E: call        __rust_alloc
  0000000141CE7853: test        rax,rax
  0000000141CE7856: je          0000000141CE7996
  0000000141CE785C: mov         rdi,rax
  0000000141CE785F: mov         qword ptr [rax],0
  0000000141CE7866: mov         qword ptr [rax+8],4
  0000000141CE786E: mov         qword ptr [rax+10h],0
  0000000141CE7876: mov         byte ptr [rax+18h],0
  0000000141CE787A: mov         rcx,qword ptr [rbx+60h]
  0000000141CE787E: jmp         0000000141CE7882
  0000000141CE7880: pause
  0000000141CE7882: mov         rax,qword ptr [rcx+8]
  0000000141CE7886: nop         word ptr cs:[rax+rax]
  0000000141CE7890: cmp         rax,0FFFFFFFFFFFFFFFFh
  0000000141CE7894: je          0000000141CE7880
  0000000141CE7896: test        rax,rax
  0000000141CE7899: js          0000000141CE7906
  0000000141CE789B: lea         rdx,[rax+1]
  0000000141CE789F: lock cmpxchg qword ptr [rcx+8],rdx
  0000000141CE78A5: jne         0000000141CE7890
  0000000141CE78A7: mov         qword ptr [r14],rcx
  0000000141CE78AA: mov         qword ptr [r14+8],rdi
  0000000141CE78AE: test        rdi,rdi
  0000000141CE78B1: jne         0000000141CE74E6
  0000000141CE78B7: lea         rcx,[anon.1d45781fa97d71a5bd78e3909e6d9c62.3.llvm.8780030650762718561]
  0000000141CE78BE: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  0000000141CE78C3: jmp         0000000141CE79A5
  0000000141CE78C8: test        rax,rax
  0000000141CE78CB: jne         0000000141CE78FA
  0000000141CE78CD: xor         edx,edx
  0000000141CE78CF: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CE78D4: mov         rcx,rax
  0000000141CE78D7: test        rax,rax
  0000000141CE78DA: jne         0000000141CE77A9
  0000000141CE78E0: jmp         0000000141CE78FA
  0000000141CE78E2: test        rax,rax
  0000000141CE78E5: jne         0000000141CE78FA
  0000000141CE78E7: xor         edx,edx
  0000000141CE78E9: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h4e917660cfe56d99E
  0000000141CE78EE: mov         rcx,rax
  0000000141CE78F1: test        rax,rax
  0000000141CE78F4: jne         0000000141CE73EC
  0000000141CE78FA: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.1.llvm.13421880302868465498]
  0000000141CE7901: call        _ZN3std6thread5local18panic_access_error17hcff639665d708376E
  0000000141CE7906: lea         rcx,[anon.9dd2267110a53640efc6a391e48173f9.1.llvm.11472377566680991268]
  0000000141CE790D: lea         rdx,[anon.9dd2267110a53640efc6a391e48173f9.3.llvm.11472377566680991268]
  0000000141CE7914: call        _ZN5alloc4sync16Arc$LT$T$C$A$GT$9downgrade18panic_cold_display17h8a92c5022fe6434eE
  0000000141CE7919: jmp         0000000141CE79A5
  0000000141CE791E: lea         rcx,[anon.f0ee56d361f9c3fe9bfb6ea081d48b1e.3.llvm.13421880302868465498]
  0000000141CE7925: call        _ZN4core4cell22panic_already_borrowed17h54fda569c7a6ddb1E
  0000000141CE792A: mov         qword ptr [rbp+30h],0
  0000000141CE7932: lea         rax,[anon.78d3d481e5a5afc01d058fe84fbba446.2.llvm.11526520697743150770]
  0000000141CE7939: mov         qword ptr [rsp+20h],rax
  0000000141CE793E: lea         rdx,[rbp+98h]
  0000000141CE7945: lea         r9,[rbp+30h]
  0000000141CE7949: xor         ecx,ecx
  0000000141CE794B: mov         r8,r14
  0000000141CE794E: call        _ZN4core9panicking13assert_failed17h83a3a5a8c66e5aabE
  0000000141CE7953: jmp         0000000141CE79A5
  0000000141CE7955: mov         r14,rcx
  0000000141CE7958: mov         r8d,3B9ACA00h
  0000000141CE795E: call        _ZN11parking_lot9raw_mutex8RawMutex9lock_slow17h7c244b522f551b38E
  0000000141CE7963: mov         rcx,r14
  0000000141CE7966: jmp         0000000141CE74F9
  0000000141CE796B: mov         rcx,rsi
  0000000141CE796E: xor         edx,edx
  0000000141CE7970: call        _ZN11parking_lot9raw_mutex8RawMutex11unlock_slow17h56cb6df3e32a2657E
  0000000141CE7975: jmp         0000000141CE7786
  0000000141CE797A: lea         rcx,[142F36278h]
  0000000141CE7981: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  0000000141CE7986: jmp         0000000141CE79A5
  0000000141CE7988: lea         rcx,[anon.1d45781fa97d71a5bd78e3909e6d9c62.4.llvm.8780030650762718561]
  0000000141CE798F: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  0000000141CE7994: jmp         0000000141CE79A5
  0000000141CE7996: mov         ecx,8
  0000000141CE799B: mov         edx,20h
  0000000141CE79A0: call        _ZN5alloc5alloc18handle_alloc_error17h3e7daf9bcd04547aE
  0000000141CE79A5: ud2
  0000000141CE79A7: nop         word ptr [rax+rax]
  0000000141CE79B0: mov         qword ptr [rsp+10h],rdx
  0000000141CE79B5: push        rbp
  0000000141CE79B6: push        r15
  0000000141CE79B8: push        r14
  0000000141CE79BA: push        r13
  0000000141CE79BC: push        r12
  0000000141CE79BE: push        rsi
  0000000141CE79BF: push        rdi
  0000000141CE79C0: push        rbx
  0000000141CE79C1: sub         rsp,0D8h
  0000000141CE79C8: lea         rbp,[rdx+80h]
  0000000141CE79CF: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE79D5: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE79DB: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE79E1: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE79E7: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE79ED: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE79F6: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE79FF: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE7A08: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7A10: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7A18: mov         rax,qword ptr [rbp+110h]
  0000000141CE7A1F: inc         qword ptr [rax]
  0000000141CE7A22: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE7A2A: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE7A32: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE7A3B: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7A44: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE7A4D: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7A53: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE7A59: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE7A5F: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7A65: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE7A6B: add         rsp,0D8h
  0000000141CE7A72: pop         rbx
  0000000141CE7A73: pop         rdi
  0000000141CE7A74: pop         rsi
  0000000141CE7A75: pop         r12
  0000000141CE7A77: pop         r13
  0000000141CE7A79: pop         r14
  0000000141CE7A7B: pop         r15
  0000000141CE7A7D: pop         rbp
  0000000141CE7A7E: ret
  0000000141CE7A7F: nop
  0000000141CE7A80: mov         qword ptr [rsp+10h],rdx
  0000000141CE7A85: push        rbp
  0000000141CE7A86: push        r15
  0000000141CE7A88: push        r14
  0000000141CE7A8A: push        r13
  0000000141CE7A8C: push        r12
  0000000141CE7A8E: push        rsi
  0000000141CE7A8F: push        rdi
  0000000141CE7A90: push        rbx
  0000000141CE7A91: sub         rsp,0D8h
  0000000141CE7A98: lea         rbp,[rdx+80h]
  0000000141CE7A9F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE7AA5: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE7AAB: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE7AB1: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE7AB7: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE7ABD: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE7AC6: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE7ACF: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE7AD8: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7AE0: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7AE8: cmp         qword ptr [rbp+100h],0
  0000000141CE7AF0: je          0000000141CE7B21
  0000000141CE7AF2: lea         rax,[rbp+108h]
  0000000141CE7AF9: mov         qword ptr [rbp+0F8h],rax
  0000000141CE7B00: lea         rax,[rbp+0F8h]
  0000000141CE7B07: mov         qword ptr [rbp+0F0h],rax
  0000000141CE7B0E: lea         rcx,[142F36140h]
  0000000141CE7B15: lea         rdx,[rbp+0F0h]
  0000000141CE7B1C: call        _ZN3std6thread5local17LocalKey$LT$T$GT$4with17h467a4e8d8d5a85fdE
  0000000141CE7B21: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE7B29: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE7B31: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE7B3A: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7B43: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE7B4C: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7B52: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE7B58: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE7B5E: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7B64: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE7B6A: add         rsp,0D8h
  0000000141CE7B71: pop         rbx
  0000000141CE7B72: pop         rdi
  0000000141CE7B73: pop         rsi
  0000000141CE7B74: pop         r12
  0000000141CE7B76: pop         r13
  0000000141CE7B78: pop         r14
  0000000141CE7B7A: pop         r15
  0000000141CE7B7C: pop         rbp
  0000000141CE7B7D: ret
  0000000141CE7B7E: nop
  0000000141CE7B80: mov         qword ptr [rsp+10h],rdx
  0000000141CE7B85: push        rbp
  0000000141CE7B86: push        r15
  0000000141CE7B88: push        r14
  0000000141CE7B8A: push        r13
  0000000141CE7B8C: push        r12
  0000000141CE7B8E: push        rsi
  0000000141CE7B8F: push        rdi
  0000000141CE7B90: push        rbx
  0000000141CE7B91: sub         rsp,0D8h
  0000000141CE7B98: lea         rbp,[rdx+80h]
  0000000141CE7B9F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE7BA5: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE7BAB: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE7BB1: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE7BB7: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE7BBD: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE7BC6: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE7BCF: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE7BD8: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7BE0: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7BE8: mov         rax,qword ptr [rbp+110h]
  0000000141CE7BEF: inc         qword ptr [rax]
  0000000141CE7BF2: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE7BFA: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE7C02: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE7C0B: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7C14: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE7C1D: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7C23: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE7C29: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE7C2F: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7C35: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE7C3B: add         rsp,0D8h
  0000000141CE7C42: pop         rbx
  0000000141CE7C43: pop         rdi
  0000000141CE7C44: pop         rsi
  0000000141CE7C45: pop         r12
  0000000141CE7C47: pop         r13
  0000000141CE7C49: pop         r14
  0000000141CE7C4B: pop         r15
  0000000141CE7C4D: pop         rbp
  0000000141CE7C4E: ret
  0000000141CE7C4F: nop
  0000000141CE7C50: mov         qword ptr [rsp+10h],rdx
  0000000141CE7C55: push        rbp
  0000000141CE7C56: push        r15
  0000000141CE7C58: push        r14
  0000000141CE7C5A: push        r13
  0000000141CE7C5C: push        r12
  0000000141CE7C5E: push        rsi
  0000000141CE7C5F: push        rdi
  0000000141CE7C60: push        rbx
  0000000141CE7C61: sub         rsp,0D8h
  0000000141CE7C68: lea         rbp,[rdx+80h]
  0000000141CE7C6F: movaps      xmmword ptr [rsp+30h],xmm15
  0000000141CE7C75: movaps      xmmword ptr [rsp+40h],xmm14
  0000000141CE7C7B: movaps      xmmword ptr [rsp+50h],xmm13
  0000000141CE7C81: movaps      xmmword ptr [rsp+60h],xmm12
  0000000141CE7C87: movaps      xmmword ptr [rsp+70h],xmm11
  0000000141CE7C8D: movaps      xmmword ptr [rsp+80h],xmm10
  0000000141CE7C96: movaps      xmmword ptr [rsp+90h],xmm9
  0000000141CE7C9F: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000141CE7CA8: movaps      xmmword ptr [rsp+0B0h],xmm7
  0000000141CE7CB0: movaps      xmmword ptr [rsp+0C0h],xmm6
  0000000141CE7CB8: lea         rcx,[rbp+98h]
  0000000141CE7CBF: call        _ZN4core3ptr146drop_in_place$LT$country_core..render..sparse_instance_buffer..SparseInstanceBufferWriter$LT$country_core..resources..sparkle..SparkleSsbo$GT$$GT$17h8d068b1c774b1660E
  0000000141CE7CC4: movaps      xmm6,xmmword ptr [rsp+0C0h]
  0000000141CE7CCC: movaps      xmm7,xmmword ptr [rsp+0B0h]
  0000000141CE7CD4: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000141CE7CDD: movaps      xmm9,xmmword ptr [rsp+90h]
  0000000141CE7CE6: movaps      xmm10,xmmword ptr [rsp+80h]
  0000000141CE7CEF: movaps      xmm11,xmmword ptr [rsp+70h]
  0000000141CE7CF5: movaps      xmm12,xmmword ptr [rsp+60h]
  0000000141CE7CFB: movaps      xmm13,xmmword ptr [rsp+50h]
  0000000141CE7D01: movaps      xmm14,xmmword ptr [rsp+40h]
  0000000141CE7D07: movaps      xmm15,xmmword ptr [rsp+30h]
  0000000141CE7D0D: add         rsp,0D8h
  0000000141CE7D14: pop         rbx
  0000000141CE7D15: pop         rdi
  0000000141CE7D16: pop         rsi
  0000000141CE7D17: pop         r12
  0000000141CE7D19: pop         r13
  0000000141CE7D1B: pop         r14
  0000000141CE7D1D: pop         r15
  0000000141CE7D1F: pop         rbp
  0000000141CE7D20: ret
  0000000141CE7D21: CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC     ...............

