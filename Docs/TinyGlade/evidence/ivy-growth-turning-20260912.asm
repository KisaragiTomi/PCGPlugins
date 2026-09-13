; Tiny Glade ivy 生长/转向相关反汇编（dumpbin /DISASM:NOBYTES + PDB，2026-09-12）
; 源: D:/MyProject/Tiny Glade/tiny-glade.exe (58 460 160 B, 2025-10-08) + tiny_glade.pdb
; 过滤器: scratchpad/grab_funcs.py（顶格标签 + 关键词），只保留与
;        「藤在障碍处怎么转向」直接相关的函数体。浮点常量是单精度、单位是米。
; ===== _ZN16system_decorator24remove_ivy_under_windows24remove_ivy_under_windows17h52ca37f59ca0fd1eE:
  0000000140814860: push        r15
  0000000140814862: push        r14
  0000000140814864: push        r13
  0000000140814866: push        r12
  0000000140814868: push        rsi
  0000000140814869: push        rdi
  000000014081486A: push        rbp
  000000014081486B: push        rbx
  000000014081486C: sub         rsp,128h
  0000000140814873: movaps      xmmword ptr [rsp+110h],xmm10
  000000014081487C: movaps      xmmword ptr [rsp+100h],xmm9
  0000000140814885: movdqa      xmmword ptr [rsp+0F0h],xmm8
  000000014081488F: movaps      xmmword ptr [rsp+0E0h],xmm7
  0000000140814897: movaps      xmmword ptr [rsp+0D0h],xmm6
  000000014081489F: mov         qword ptr [rsp+80h],r9
  00000001408148A7: mov         rbx,r8
  00000001408148AA: mov         r14,rcx
  00000001408148AD: mov         rsi,qword ptr [rsp+190h]
  00000001408148B5: mov         rcx,rdx
  00000001408148B8: call        _ZN203_$LT$country_core..resources..walls..decorator_subtype..DerivedDecoratorInfo$u20$as$u20$core..convert..From$LT$$RF$country_core..resources..walls..decorator_subtype..DerivedDecoratorInfoResources$GT$$GT$4from17h02f6307590e1ffc4E
  00000001408148BD: mov         qword ptr [rsp+98h],rax
  00000001408148C5: mov         r15,qword ptr [rbx]
  00000001408148C8: mov         rax,qword ptr [rbx+8]
  00000001408148CC: mov         r8,qword ptr [r15]
  00000001408148CF: xor         edi,edi
  00000001408148D1: mov         rcx,r8
  00000001408148D4: sub         rcx,qword ptr [rax+18h]
  00000001408148D8: cmovb       rcx,rdi
  00000001408148DC: sub         r8,qword ptr [rax+38h]
  00000001408148E0: mov         qword ptr [rsp+90h],rdx
  00000001408148E8: cmovb       r8,rdi
  00000001408148EC: lea         r9,[rcx+rcx*8]
  00000001408148F0: shl         r9,4
  00000001408148F4: add         r9,qword ptr [rax+8]
  00000001408148F8: mov         rdx,qword ptr [rax+10h]
  00000001408148FC: mov         r10,qword ptr [rax+30h]
  0000000140814900: sub         rdx,rcx
  0000000140814903: cmovb       rdx,rdi
  0000000140814907: mov         r11d,8
  000000014081490D: cmovb       r9,r11
  0000000140814911: lea         rcx,[r8+r8*8]
  0000000140814915: shl         rcx,4
  0000000140814919: add         rcx,qword ptr [rax+28h]
  000000014081491D: sub         r10,r8
  0000000140814920: cmovb       r10,rdi
  0000000140814924: cmovb       rcx,r11
  0000000140814928: mov         rax,qword ptr [rax+40h]
  000000014081492C: sub         rax,r10
  000000014081492F: sub         rax,rdx
  0000000140814932: mov         qword ptr [r15],rax
  0000000140814935: lea         rdi,[rdx+rdx*8]
  0000000140814939: shl         rdi,4
  000000014081493D: add         rdi,r9
  0000000140814940: lea         rax,[r10+r10*8]
  0000000140814944: xor         r10d,r10d
  0000000140814947: shl         rax,4
  000000014081494B: add         rax,rcx
  000000014081494E: mov         qword ptr [rsp+0B8h],rax
  0000000140814956: mov         rax,qword ptr [r14]
  0000000140814959: mov         qword ptr [rsp+0A0h],rax
  0000000140814961: mov         rax,qword ptr [rsi]
  0000000140814964: mov         qword ptr [rsp+88h],rax
  000000014081496C: pcmpeqd     xmm8,xmm8
  0000000140814971: xorps       xmm9,xmm9
  0000000140814975: movss       xmm10,dword ptr [__real@3f800000]
  000000014081497E: jmp         0000000140814AC0
  0000000140814983: nop         word ptr cs:[rax+rax]
  0000000140814990: movdqu      xmm1,xmmword ptr [rcx+r8]
  0000000140814996: movdqa      xmm2,xmm1
  000000014081499A: pcmpeqb     xmm2,xmm0
  000000014081499E: pmovmskb    r10d,xmm2
  00000001408149A3: test        r10d,r10d
  00000001408149A6: je          00000001408149E0
  00000001408149A8: tzcnt       r11d,r10d
  00000001408149AD: add         r11,r8
  00000001408149B0: and         r11,rdx
  00000001408149B3: neg         r11
  00000001408149B6: lea         r11,[r11+r11*4]
  00000001408149BA: shl         r11,4
  00000001408149BE: cmp         ebp,dword ptr [rax+r11]
  00000001408149C2: je          0000000140814A03
  00000001408149C4: lea         r11d,[r10-1]
  00000001408149C8: and         r11w,r10w
  00000001408149CC: mov         r10d,r11d
  00000001408149CF: jne         00000001408149A8
  00000001408149D1: nop         word ptr cs:[rax+rax]
  00000001408149E0: pcmpeqb     xmm1,xmm8
  00000001408149E5: pmovmskb    r10d,xmm1
  00000001408149EA: test        r10d,r10d
  00000001408149ED: jne         0000000140814E1D
  00000001408149F3: add         r8,r9
  00000001408149F6: add         r8,10h
  00000001408149FA: add         r9,10h
  00000001408149FE: and         r8,rdx
  0000000140814A01: jmp         0000000140814990
  0000000140814A03: mov         rdx,qword ptr [rsp+88h]
  0000000140814A0B: mov         rax,qword ptr [rdx+8]
  0000000140814A0F: mov         rcx,qword ptr [rdx+10h]
  0000000140814A13: add         rcx,rcx
  0000000140814A16: mov         qword ptr [rsp+50h],rax
  0000000140814A1B: mov         qword ptr [rsp+58h],rcx
  0000000140814A20: movsd       xmm0,mmword ptr [rdx+50h]
  0000000140814A25: movsd       mmword ptr [rsp+60h],xmm0
  0000000140814A2B: lea         rcx,[rsp+50h]
  0000000140814A30: movaps      xmm1,xmm6
  0000000140814A33: movaps      xmm2,xmm7
  0000000140814A36: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  0000000140814A3B: movaps      xmm1,xmm0
  0000000140814A3E: mulss       xmm1,xmm9
  0000000140814A43: subss       xmm7,xmm1
  0000000140814A47: unpcklps    xmm1,xmm0
  0000000140814A4A: subps       xmm6,xmm1
  0000000140814A4D: movlps      qword ptr [rsp+40h],xmm6
  0000000140814A52: movss       dword ptr [rsp+48h],xmm7
  0000000140814A58: mov         ecx,ebp
  0000000140814A5A: mov         rdx,qword ptr [rsp+98h]
  0000000140814A62: call        _ZN146_$LT$country_core..resources..walls..decorator..DecoratorId$u20$as$u20$country_core..resources..walls..decorator_subtype..IntoDecoratorSubtype$GT$22into_decorator_subtype17h827ad78aa35f2e95E
  0000000140814A67: mov         ecx,r13d
  0000000140814A6A: mov         edx,eax
  0000000140814A6C: call        _ZN12country_core9resources5walls17decorator_subtype15DecoratorLibKey3new17hce67db27b83b546aE
  0000000140814A71: mov         byte ptr [rsp+3Eh],al
  0000000140814A75: mov         byte ptr [rsp+3Fh],dl
  0000000140814A79: mov         rcx,qword ptr [rsp+90h]
  0000000140814A81: lea         rdx,[rsp+3Eh]
  0000000140814A86: call        _ZN3std11collections4hash3map24HashMap$LT$K$C$V$C$S$GT$3get17he63405d73122ed57E.llvm.7518301806615881319
  0000000140814A8B: test        rax,rax
  0000000140814A8E: je          0000000140814E75
  0000000140814A94: movss       xmm6,dword ptr [rax+3Ch]
  0000000140814A99: subss       xmm6,dword ptr [rax+38h]
  0000000140814A9E: movss       xmm7,dword ptr [rax+30h]
  0000000140814AA3: addss       xmm7,xmm9
  0000000140814AA8: addss       xmm6,xmm10
  0000000140814AAD: lea         rcx,[rsp+0C0h]
  0000000140814AB5: jmp         0000000140814DF3
  0000000140814ABA: nop         word ptr [rax+rax]
  0000000140814AC0: test        r9,r9
  0000000140814AC3: je          0000000140814AE0
  0000000140814AC5: cmp         r9,rdi
  0000000140814AC8: je          0000000140814AE0
  0000000140814ACA: lea         rsi,[r9+90h]
  0000000140814AD1: mov         r14,rcx
  0000000140814AD4: mov         r12,r9
  0000000140814AD7: jmp         0000000140814B06
  0000000140814AD9: nop         dword ptr [rax]
  0000000140814AE0: test        rcx,rcx
  0000000140814AE3: je          0000000140814E36
  0000000140814AE9: cmp         rcx,qword ptr [rsp+0B8h]
  0000000140814AF1: je          0000000140814E36
  0000000140814AF7: lea         r14,[rcx+90h]
  0000000140814AFE: xor         esi,esi
  0000000140814B00: mov         r12,rcx
  0000000140814B03: mov         rcx,r14
  0000000140814B06: inc         qword ptr [r15]
  0000000140814B09: mov         rax,qword ptr [r12]
  0000000140814B0D: lea         rdx,[rax-3]
  0000000140814B11: add         rax,0FFFFFFFFFFFFFFFEh
  0000000140814B15: cmp         rdx,2
  0000000140814B19: cmovae      rax,r10
  0000000140814B1D: mov         r9,rsi
  0000000140814B20: cmp         rax,2
  0000000140814B24: je          0000000140814AC0
  0000000140814B26: cmp         rax,1
  0000000140814B2A: jne         0000000140814C10
  0000000140814B30: mov         ebp,dword ptr [r12+40h]
  0000000140814B35: mov         dword ptr [rsp+4Ch],ebp
  0000000140814B39: movsd       xmm6,mmword ptr [r12+2Ch]
  0000000140814B40: movss       xmm7,dword ptr [r12+34h]
  0000000140814B47: movzx       eax,byte ptr [r12+3Ah]
  0000000140814B4D: movzx       r13d,byte ptr [r12+45h]
  0000000140814B53: mov         rdx,qword ptr [r12+8]
  0000000140814B58: mov         r8,qword ptr [r12+10h]
  0000000140814B5D: mov         qword ptr [rsp+0C0h],rdx
  0000000140814B65: mov         qword ptr [rsp+0C8h],r8
  0000000140814B6D: cmp         byte ptr [r12+80h],0
  0000000140814B76: mov         r9,rsi
  0000000140814B79: mov         rcx,r14
  0000000140814B7C: jne         0000000140814AC0
  0000000140814B82: add         al,0FEh
  0000000140814B84: mov         r9,rsi
  0000000140814B87: mov         rcx,r14
  0000000140814B8A: cmp         al,3
  0000000140814B8C: jb          0000000140814AC0
  0000000140814B92: mov         rcx,qword ptr [rsp+0A0h]
  0000000140814B9A: call        _ZN12country_core9resources5walls17decorator_storage16DecoratorStorage3get17h3de092ab5378c7c2E
  0000000140814B9F: xor         r10d,r10d
  0000000140814BA2: mov         r9,rsi
  0000000140814BA5: mov         rcx,r14
  0000000140814BA8: test        rax,rax
  0000000140814BAB: je          0000000140814AC0
  0000000140814BB1: cmp         qword ptr [rax+48h],0
  0000000140814BB6: mov         r9,rsi
  0000000140814BB9: mov         rcx,r14
  0000000140814BBC: je          0000000140814AC0
  0000000140814BC2: mov         rcx,rax
  0000000140814BC5: add         rcx,50h
  0000000140814BC9: lea         rdx,[rsp+4Ch]
  0000000140814BCE: mov         rbx,rax
  0000000140814BD1: call        _ZN4core4hash11BuildHasher8hash_one17h0d26b741781cef8eE
  0000000140814BD6: mov         rcx,qword ptr [rbx+30h]
  0000000140814BDA: mov         rdx,qword ptr [rbx+38h]
  0000000140814BDE: mov         r8,rdx
  0000000140814BE1: and         r8,rax
  0000000140814BE4: shr         rax,39h
  0000000140814BE8: movd        xmm0,eax
  0000000140814BEC: punpcklbw   xmm0,xmm0
  0000000140814BF0: pshuflw     xmm0,xmm0,0
  0000000140814BF5: pshufd      xmm0,xmm0,0
  0000000140814BFA: lea         rax,[rcx-50h]
  0000000140814BFE: xor         r9d,r9d
  0000000140814C01: jmp         0000000140814990
  0000000140814C06: nop         word ptr cs:[rax+rax]
  0000000140814C10: mov         rdx,qword ptr [r12+10h]
  0000000140814C15: mov         r9,rsi
  0000000140814C18: mov         rcx,r14
  0000000140814C1B: cmp         rdx,3
  0000000140814C1F: je          0000000140814AC0
  0000000140814C25: mov         r8,qword ptr [r12+18h]
  0000000140814C2A: mov         rcx,qword ptr [rsp+0A0h]
  0000000140814C32: call        _ZN12country_core9resources5walls17decorator_storage16DecoratorStorage3get17h3de092ab5378c7c2E
  0000000140814C37: xor         r10d,r10d
  0000000140814C3A: mov         r9,rsi
  0000000140814C3D: mov         rcx,r14
  0000000140814C40: test        rax,rax
  0000000140814C43: je          0000000140814AC0
  0000000140814C49: cmp         qword ptr [rax+48h],0
  0000000140814C4E: mov         r9,rsi
  0000000140814C51: mov         rcx,r14
  0000000140814C54: je          0000000140814AC0
  0000000140814C5A: lea         r13,[r12+68h]
  0000000140814C5F: mov         rcx,rax
  0000000140814C62: add         rcx,50h
  0000000140814C66: mov         rdx,r13
  0000000140814C69: mov         rbx,rax
  0000000140814C6C: call        _ZN4core4hash11BuildHasher8hash_one17h0d26b741781cef8eE
  0000000140814C71: mov         rbp,qword ptr [rbx+30h]
  0000000140814C75: mov         rcx,qword ptr [rbx+38h]
  0000000140814C79: mov         rdx,rcx
  0000000140814C7C: and         rdx,rax
  0000000140814C7F: shr         rax,39h
  0000000140814C83: movd        xmm0,eax
  0000000140814C87: punpcklbw   xmm0,xmm0
  0000000140814C8B: pshuflw     xmm0,xmm0,0
  0000000140814C90: pshufd      xmm0,xmm0,0
  0000000140814C95: lea         rax,[rbp-50h]
  0000000140814C99: xor         r8d,r8d
  0000000140814C9C: movdqu      xmm1,xmmword ptr [rbp+rdx]
  0000000140814CA2: movdqa      xmm2,xmm1
  0000000140814CA6: pcmpeqb     xmm2,xmm0
  0000000140814CAA: pmovmskb    r9d,xmm2
  0000000140814CAF: test        r9d,r9d
  0000000140814CB2: je          0000000140814CF0
  0000000140814CB4: mov         r10d,dword ptr [r13]
  0000000140814CB8: tzcnt       r11d,r9d
  0000000140814CBD: add         r11,rdx
  0000000140814CC0: and         r11,rcx
  0000000140814CC3: neg         r11
  0000000140814CC6: lea         rbx,[r11+r11*4]
  0000000140814CCA: shl         rbx,4
  0000000140814CCE: cmp         r10d,dword ptr [rax+rbx]
  0000000140814CD2: je          0000000140814D19
  0000000140814CD4: lea         r11d,[r9-1]
  0000000140814CD8: and         r11w,r9w
  0000000140814CDC: mov         r9d,r11d
  0000000140814CDF: jne         0000000140814CB8
  0000000140814CE1: nop         word ptr cs:[rax+rax]
  0000000140814CF0: pcmpeqb     xmm1,xmm8
  0000000140814CF5: pmovmskb    r9d,xmm1
  0000000140814CFA: test        r9d,r9d
  0000000140814CFD: mov         r10d,0
  0000000140814D03: jne         0000000140814E2B
  0000000140814D09: add         rdx,r8
  0000000140814D0C: add         rdx,10h
  0000000140814D10: add         r8,10h
  0000000140814D14: and         rdx,rcx
  0000000140814D17: jmp         0000000140814C9C
  0000000140814D19: cmp         byte ptr [r12+42h],1
  0000000140814D1F: mov         r9,rsi
  0000000140814D22: mov         rcx,r14
  0000000140814D25: mov         r10d,0
  0000000140814D2B: ja          0000000140814AC0
  0000000140814D31: movss       xmm6,dword ptr [r12+3Ch]
  0000000140814D38: mov         rdx,qword ptr [rsp+88h]
  0000000140814D40: mov         rax,qword ptr [rdx+8]
  0000000140814D44: mov         rcx,qword ptr [rdx+10h]
  0000000140814D48: add         rcx,rcx
  0000000140814D4B: mov         qword ptr [rsp+50h],rax
  0000000140814D50: mov         qword ptr [rsp+58h],rcx
  0000000140814D55: movsd       xmm0,mmword ptr [rdx+50h]
  0000000140814D5A: movsd       mmword ptr [rsp+60h],xmm0
  0000000140814D60: movsd       xmm7,mmword ptr [r12+34h]
  0000000140814D67: lea         rcx,[rsp+50h]
  0000000140814D6C: movaps      xmm1,xmm7
  0000000140814D6F: movaps      xmm2,xmm6
  0000000140814D72: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  0000000140814D77: movaps      xmm1,xmm0
  0000000140814D7A: mulss       xmm1,xmm9
  0000000140814D7F: subss       xmm6,xmm1
  0000000140814D83: unpcklps    xmm1,xmm0
  0000000140814D86: subps       xmm7,xmm1
  0000000140814D89: movlps      qword ptr [rsp+40h],xmm7
  0000000140814D8E: movss       dword ptr [rsp+48h],xmm6
  0000000140814D94: mov         ecx,dword ptr [r12+68h]
  0000000140814D99: movzx       ebp,byte ptr [rbp+rbx-4]
  0000000140814D9E: mov         rdx,qword ptr [rsp+98h]
  0000000140814DA6: call        _ZN146_$LT$country_core..resources..walls..decorator..DecoratorId$u20$as$u20$country_core..resources..walls..decorator_subtype..IntoDecoratorSubtype$GT$22into_decorator_subtype17h827ad78aa35f2e95E
  0000000140814DAB: mov         ecx,ebp
  0000000140814DAD: mov         edx,eax
  0000000140814DAF: call        _ZN12country_core9resources5walls17decorator_subtype15DecoratorLibKey3new17hce67db27b83b546aE
  0000000140814DB4: mov         byte ptr [rsp+3Eh],al
  0000000140814DB8: mov         byte ptr [rsp+3Fh],dl
  0000000140814DBC: mov         rcx,qword ptr [rsp+90h]
  0000000140814DC4: lea         rdx,[rsp+3Eh]
  0000000140814DC9: call        _ZN3std11collections4hash3map24HashMap$LT$K$C$V$C$S$GT$3get17he63405d73122ed57E.llvm.7518301806615881319
  0000000140814DCE: test        rax,rax
  0000000140814DD1: je          0000000140814E75
  0000000140814DD7: movss       xmm6,dword ptr [rax+3Ch]
  0000000140814DDC: subss       xmm6,dword ptr [rax+38h]
  0000000140814DE1: movss       xmm7,dword ptr [rax+30h]
  0000000140814DE6: addss       xmm7,xmm9
  0000000140814DEB: addss       xmm6,xmm10
  0000000140814DF0: mov         rcx,r12
  0000000140814DF3: call        _ZN12country_core9resources5walls17decorator_storage20DecoratorStorageAddr11try_as_wall17hf38d397ec269be05E
  0000000140814DF8: mov         rcx,qword ptr [rsp+80h]
  0000000140814E00: mov         qword ptr [rsp+28h],rcx
  0000000140814E05: mov         qword ptr [rsp+20h],rdx
  0000000140814E0A: lea         rcx,[rsp+40h]
  0000000140814E0F: movaps      xmm1,xmm7
  0000000140814E12: movaps      xmm2,xmm6
  0000000140814E15: mov         r9,rax
  0000000140814E18: call        _ZN12country_core7systems3ivy10ivy_pruner22remove_ivy_at_location17h81e469fca5672e93E
  0000000140814E1D: mov         r9,rsi
  0000000140814E20: mov         rcx,r14
  0000000140814E23: xor         r10d,r10d
  0000000140814E26: jmp         0000000140814AC0
  0000000140814E2B: mov         r9,rsi
  0000000140814E2E: mov         rcx,r14
  0000000140814E31: jmp         0000000140814AC0
  0000000140814E36: movaps      xmm6,xmmword ptr [rsp+0D0h]
  0000000140814E3E: movaps      xmm7,xmmword ptr [rsp+0E0h]
  0000000140814E46: movaps      xmm8,xmmword ptr [rsp+0F0h]
  0000000140814E4F: movaps      xmm9,xmmword ptr [rsp+100h]
  0000000140814E58: movaps      xmm10,xmmword ptr [rsp+110h]
  0000000140814E61: add         rsp,128h
  0000000140814E68: pop         rbx
  0000000140814E69: pop         rbp
  0000000140814E6A: pop         rdi
  0000000140814E6B: pop         rsi
  0000000140814E6C: pop         r12
  0000000140814E6E: pop         r13
  0000000140814E70: pop         r14
  0000000140814E72: pop         r15
  0000000140814E74: ret
  0000000140814E75: lea         rax,[rsp+3Eh]
  0000000140814E7A: mov         qword ptr [rsp+0A8h],rax
  0000000140814E82: lea         rax,[_ZN103_$LT$country_core..resources..walls..decorator_subtype..DecoratorLibKey$u20$as$u20$core..fmt..Debug$GT$3fmt17h6dabd3b8456e706fE.llvm.7518301806615881319]
  0000000140814E89: mov         qword ptr [rsp+0B0h],rax
  0000000140814E91: lea         rax,[anon.aaa2ffd9936f24d9e01572d66e2542e3.34.llvm.7518301806615881319]
  0000000140814E98: mov         qword ptr [rsp+50h],rax
  0000000140814E9D: mov         qword ptr [rsp+58h],1
  0000000140814EA6: mov         qword ptr [rsp+70h],0
  0000000140814EAF: lea         rax,[rsp+0A8h]
  0000000140814EB7: mov         qword ptr [rsp+60h],rax
  0000000140814EBC: mov         qword ptr [rsp+68h],1
  0000000140814EC5: lea         rdx,[anon.aaa2ffd9936f24d9e01572d66e2542e3.35.llvm.7518301806615881319]
  0000000140814ECC: lea         rcx,[rsp+50h]
  0000000140814ED1: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  0000000140814ED6: int         3
  0000000140814ED7: CC CC CC CC CC CC CC CC CC                       .........

; ===== _ZN12country_core7systems3ivy10ivy_grower10ivy_grower17h74efdc1b62dfcc21E:
  000000014094A910: push        rbp
  000000014094A911: push        r15
  000000014094A913: push        r14
  000000014094A915: push        r13
  000000014094A917: push        r12
  000000014094A919: push        rsi
  000000014094A91A: push        rdi
  000000014094A91B: push        rbx
  000000014094A91C: sub         rsp,2F8h
  000000014094A923: lea         rbp,[rsp+80h]
  000000014094A92B: movaps      xmmword ptr [rbp+260h],xmm15
  000000014094A933: movaps      xmmword ptr [rbp+250h],xmm14
  000000014094A93B: movaps      xmmword ptr [rbp+240h],xmm13
  000000014094A943: movaps      xmmword ptr [rbp+230h],xmm12
  000000014094A94B: movaps      xmmword ptr [rbp+220h],xmm11
  000000014094A953: movaps      xmmword ptr [rbp+210h],xmm10
  000000014094A95B: movaps      xmmword ptr [rbp+200h],xmm9
  000000014094A963: movaps      xmmword ptr [rbp+1F0h],xmm8
  000000014094A96B: movaps      xmmword ptr [rbp+1E0h],xmm7
  000000014094A972: movaps      xmmword ptr [rbp+1D0h],xmm6
  000000014094A979: mov         qword ptr [rbp+1C8h],0FFFFFFFFFFFFFFFEh
  000000014094A984: mov         rsi,r9
  000000014094A987: mov         qword ptr [rbp-8],r8
  000000014094A98B: mov         qword ptr [rbp+190h],rdx
  000000014094A992: mov         r14,rcx
  000000014094A995: mov         rbx,qword ptr [rbp+2F8h]
  000000014094A99C: call        _ZN6puffin13are_scopes_on17h8f511bd2e57f501aE
  000000014094A9A1: mov         r15d,eax
  000000014094A9A4: test        al,al
  000000014094A9A6: mov         qword ptr [rbp+1B8h],rsi
  000000014094A9AD: je          000000014094AA0F
  000000014094A9AF: lea         r13,[142AB3358h]
  000000014094A9B6: mov         r12d,32h
  000000014094A9BC: mov         edx,32h
  000000014094A9C1: mov         rcx,r13
  000000014094A9C4: call        0000000140948A20
  000000014094A9C9: cmp         rax,1
  000000014094A9CD: jne         000000014094AAA0
  000000014094A9D3: mov         r9,rdx
  000000014094A9D6: test        rdx,rdx
  000000014094A9D9: je          000000014094AA33
  000000014094A9DB: cmp         r9,32h
  000000014094A9DF: jae         000000014094AA31
  000000014094A9E1: lea         rax,[142AB3358h]
  000000014094A9E8: cmp         byte ptr [r9+rax],0BFh
  000000014094A9ED: jg          000000014094AA33
  000000014094A9EF: lea         rax,[142AB3458h]
  000000014094A9F6: mov         qword ptr [rsp+20h],rax
  000000014094A9FB: lea         rcx,[142AB3358h]
  000000014094AA02: mov         edx,32h
  000000014094AA07: xor         r8d,r8d
  000000014094AA0A: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  000000014094AA0F: xor         eax,eax
  000000014094AA11: mov         qword ptr [rbp+0A8h],rax
  000000014094AA18: mov         rcx,qword ptr [rbx]
  000000014094AA1B: mov         edx,dword ptr [rcx+118h]
  000000014094AA21: lea         r8,[142AB3BE0h]
  000000014094AA28: movsxd      rdx,dword ptr [r8+rdx*4]
  000000014094AA2C: add         rdx,r8
  000000014094AA2F: jmp         rdx
  000000014094AA31: jne         000000014094A9EF
  000000014094AA33: lea         r13,[142AB3358h]
  000000014094AA3A: mov         rcx,r13
  000000014094AA3D: mov         rdx,r9
  000000014094AA40: call        0000000140948A20
  000000014094AA45: cmp         rax,1
  000000014094AA49: jne         000000014094AAA0
  000000014094AA4B: mov         r8,rdx
  000000014094AA4E: add         r8,2
  000000014094AA52: je          000000014094AA8D
  000000014094AA54: cmp         r8,32h
  000000014094AA58: jae         000000014094AA8B
  000000014094AA5A: lea         rax,[142AB3358h]
  000000014094AA61: cmp         byte ptr [r8+rax],0BFh
  000000014094AA66: jg          000000014094AA8D
  000000014094AA68: lea         rax,[142AB3470h]
  000000014094AA6F: mov         qword ptr [rsp+20h],rax
  000000014094AA74: lea         rcx,[142AB3358h]
  000000014094AA7B: mov         edx,32h
  000000014094AA80: mov         r9d,32h
  000000014094AA86: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  000000014094AA8B: jne         000000014094AA68
  000000014094AA8D: mov         r12d,30h
  000000014094AA93: sub         r12,rdx
  000000014094AA96: lea         r13,[142AB3358h]
  000000014094AA9D: add         r13,r8
  000000014094AAA0: lea         rax,[142AB36B1h]
  000000014094AAA7: lea         rsi,[142AB3680h]
  000000014094AAAE: nop
  000000014094AAB0: movsx       ecx,byte ptr [rax-1]
  000000014094AAB4: test        ecx,ecx
  000000014094AAB6: js          000000014094AAD0
  000000014094AAB8: dec         rax
  000000014094AABB: cmp         ecx,5Ch
  000000014094AABE: jne         000000014094AB29
  000000014094AAC0: jmp         000000014094AB3E
  000000014094AAC2: nop         word ptr cs:[rax+rax]
  000000014094AAD0: movzx       edx,byte ptr [rax-2]
  000000014094AAD4: cmp         dl,0C0h
  000000014094AAD7: jge         000000014094AAFE
  000000014094AAD9: movzx       r8d,byte ptr [rax-3]
  000000014094AADE: cmp         r8b,0C0h
  000000014094AAE2: jge         000000014094AB07
  000000014094AAE4: movzx       r9d,byte ptr [rax-4]
  000000014094AAE9: add         rax,0FFFFFFFFFFFFFFFCh
  000000014094AAED: and         r9d,7
  000000014094AAF1: shl         r9d,6
  000000014094AAF5: and         r8d,3Fh
  000000014094AAF9: or          r8d,r9d
  000000014094AAFC: jmp         000000014094AB0F
  000000014094AAFE: add         rax,0FFFFFFFFFFFFFFFEh
  000000014094AB02: and         edx,1Fh
  000000014094AB05: jmp         000000014094AB19
  000000014094AB07: add         rax,0FFFFFFFFFFFFFFFDh
  000000014094AB0B: and         r8d,0Fh
  000000014094AB0F: shl         r8d,6
  000000014094AB13: and         edx,3Fh
  000000014094AB16: or          edx,r8d
  000000014094AB19: shl         edx,6
  000000014094AB1C: and         cl,3Fh
  000000014094AB1F: movzx       ecx,cl
  000000014094AB22: or          ecx,edx
  000000014094AB24: cmp         ecx,5Ch
  000000014094AB27: je          000000014094AB3E
  000000014094AB29: cmp         ecx,2Fh
  000000014094AB2C: je          000000014094AB3E
  000000014094AB2E: cmp         rax,rsi
  000000014094AB31: jne         000000014094AAB0
  000000014094AB37: mov         edi,31h
  000000014094AB3C: jmp         000000014094AB8D
  000000014094AB3E: lea         rsi,[142AB3680h]
  000000014094AB45: sub         rax,rsi
  000000014094AB48: mov         r8,rax
  000000014094AB4B: inc         r8
  000000014094AB4E: je          000000014094AB82
  000000014094AB50: cmp         r8,31h
  000000014094AB54: jae         000000014094AB80
  000000014094AB56: cmp         byte ptr [r8+rsi],0BFh
  000000014094AB5B: jg          000000014094AB82
  000000014094AB5D: lea         rax,[142AB3438h]
  000000014094AB64: mov         qword ptr [rsp+20h],rax
  000000014094AB69: lea         rcx,[142AB3680h]
  000000014094AB70: mov         edx,31h
  000000014094AB75: mov         r9d,31h
  000000014094AB7B: call        _ZN4core3str16slice_error_fail17h78e32f96bb5cc05dE
  000000014094AB80: jne         000000014094AB5D
  000000014094AB82: mov         edi,30h
  000000014094AB87: sub         rdi,rax
  000000014094AB8A: add         rsi,r8
  000000014094AB8D: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  000000014094AB92: mov         rcx,rax
  000000014094AB95: mov         rax,qword ptr [rax]
  000000014094AB98: cmp         rax,1
  000000014094AB9C: jne         000000014094C16E
  000000014094ABA2: add         rcx,8
  000000014094ABA6: cmp         qword ptr [rcx],0
  000000014094ABAA: jne         000000014094C1D7
  000000014094ABB0: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  000000014094ABB7: mov         qword ptr [rbp+148h],rcx
  000000014094ABBE: add         rcx,8
  000000014094ABC2: mov         qword ptr [rsp+20h],rdi
  000000014094ABC7: mov         qword ptr [rsp+30h],0
  000000014094ABD0: mov         qword ptr [rsp+28h],1
  000000014094ABD9: mov         rdx,r13
  000000014094ABDC: mov         r8,r12
  000000014094ABDF: mov         r9,rsi
  000000014094ABE2: call        _ZN6puffin14ThreadProfiler11begin_scope17h161a40482b7cfe19E
  000000014094ABE7: mov         rsi,rax
  000000014094ABEA: mov         rax,qword ptr [rbp+148h]
  000000014094ABF1: inc         qword ptr [rax]
  000000014094ABF4: mov         qword ptr [rbp+0B0h],rsi
  000000014094ABFB: mov         eax,1
  000000014094AC00: mov         qword ptr [rbp+0A8h],rax
  000000014094AC07: mov         rcx,qword ptr [rbx]
  000000014094AC0A: mov         edx,dword ptr [rcx+118h]
  000000014094AC10: lea         r8,[142AB3BE0h]
  000000014094AC17: movsxd      rdx,dword ptr [r8+rdx*4]
  000000014094AC1B: add         rdx,r8
  000000014094AC1E: jmp         rdx
  000000014094AC20: mov         rbx,qword ptr [r14]
  000000014094AC23: movzx       ecx,byte ptr [rbx+48h]
  000000014094AC27: test        cl,1
  000000014094AC2A: jne         000000014094AC51
  000000014094AC2C: jmp         000000014094C0C0
  000000014094AC31: xorps       xmm0,xmm0
  000000014094AC34: ucomiss     xmm0,dword ptr [rcx+11Ch]
  000000014094AC3B: mov         rbx,qword ptr [r14]
  000000014094AC3E: movzx       ecx,byte ptr [rbx+48h]
  000000014094AC42: jae         000000014094C0A6
  000000014094AC48: test        cl,1
  000000014094AC4B: je          000000014094C0C0
  000000014094AC51: mov         rdx,qword ptr [rbp+308h]
  000000014094AC58: mov         rcx,qword ptr [rdx]
  000000014094AC5B: mov         r8,qword ptr [rdx+8]
  000000014094AC5F: mov         r9,qword ptr [rcx]
  000000014094AC62: xor         r15d,r15d
  000000014094AC65: mov         r10,r9
  000000014094AC68: sub         r10,qword ptr [r8+18h]
  000000014094AC6C: cmovb       r10,r15
  000000014094AC70: sub         r9,qword ptr [r8+38h]
  000000014094AC74: cmovb       r9,r15
  000000014094AC78: lea         rdx,[r10+r10*2]
  000000014094AC7C: shl         rdx,4
  000000014094AC80: add         rdx,qword ptr [r8+8]
  000000014094AC84: mov         r11,qword ptr [r8+10h]
  000000014094AC88: sub         r11,r10
  000000014094AC8B: cmovb       r11,r15
  000000014094AC8F: mov         esi,8
  000000014094AC94: cmovb       rdx,rsi
  000000014094AC98: lea         r10,[r9+r9*2]
  000000014094AC9C: shl         r10,4
  000000014094ACA0: add         r10,qword ptr [r8+28h]
  000000014094ACA4: mov         rdi,qword ptr [r8+30h]
  000000014094ACA8: sub         rdi,r9
  000000014094ACAB: cmovb       rdi,r15
  000000014094ACAF: cmovb       r10,rsi
  000000014094ACB3: lea         r9,[rdi+r11]
  000000014094ACB7: mov         r8,qword ptr [r8+40h]
  000000014094ACBB: sub         r8,r9
  000000014094ACBE: mov         qword ptr [rcx],r8
  000000014094ACC1: lea         r8,[r11+r11*2]
  000000014094ACC5: shl         r8,4
  000000014094ACC9: add         r8,rdx
  000000014094ACCC: lea         r11,[rdi+rdi*2]
  000000014094ACD0: shl         r11,4
  000000014094ACD4: add         r11,r10
  000000014094ACD7: mov         qword ptr [rbp+150h],rcx
  000000014094ACDE: mov         qword ptr [rbp+158h],rdx
  000000014094ACE5: mov         qword ptr [rbp+160h],r8
  000000014094ACEC: mov         qword ptr [rbp+168h],r10
  000000014094ACF3: mov         qword ptr [rbp+170h],r11
  000000014094ACFA: mov         qword ptr [rbp+178h],r9
  000000014094AD01: mov         qword ptr [rbp+78h],rax
  000000014094AD05: lea         r8,[anon.a9f01d7c42a6b6ded4ee4033507219c5.7.llvm.1057620027058656075]
  000000014094AD0C: lea         rcx,[rbp+48h]
  000000014094AD10: lea         rdx,[rbp+150h]
  000000014094AD17: call        _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17hc7b1252d0160710aE
  000000014094AD1C: mov         rax,qword ptr [rbp+2F0h]
  000000014094AD23: mov         rcx,qword ptr [rbp+2E8h]
  000000014094AD2A: mov         rdx,qword ptr [rbp+2E0h]
  000000014094AD31: mov         qword ptr [rbp+0C8h],0
  000000014094AD3C: mov         qword ptr [rbp+0D0h],8
  000000014094AD47: mov         qword ptr [rbp+0D8h],0
  000000014094AD52: mov         r8d,dword ptr [r14+1Ch]
  000000014094AD56: mov         r9,qword ptr [r14+10h]
  000000014094AD5A: mov         dword ptr [r9],r8d
  000000014094AD5D: mov         r12,qword ptr [rbx]
  000000014094AD60: mov         r15,qword ptr [rbx+18h]
  000000014094AD64: movdqa      xmm0,xmmword ptr [r12]
  000000014094AD6A: pmovmskb    ebx,xmm0
  000000014094AD6E: not         ebx
  000000014094AD70: lea         r13,[r12+10h]
  000000014094AD75: mov         r8,qword ptr [rbp+190h]
  000000014094AD7C: mov         r8,qword ptr [r8]
  000000014094AD7F: mov         qword ptr [rbp+190h],r8
  000000014094AD86: mov         r8,qword ptr [rbp+1B8h]
  000000014094AD8D: mov         r8,qword ptr [r8]
  000000014094AD90: mov         rdi,qword ptr [rcx]
  000000014094AD93: mov         rcx,qword ptr [rdx]
  000000014094AD96: mov         qword ptr [rbp+10h],rcx
  000000014094AD9A: mov         qword ptr [rbp+1A0h],r8
  000000014094ADA1: lea         rcx,[r8+20h]
  000000014094ADA5: mov         qword ptr [rbp+88h],rcx
  000000014094ADAC: mov         ecx,dword ptr [rax+1Ch]
  000000014094ADAF: mov         dword ptr [rbp+0C4h],ecx
  000000014094ADB5: mov         rcx,qword ptr [rax]
  000000014094ADB8: mov         rax,qword ptr [rax+10h]
  000000014094ADBC: mov         qword ptr [rbp],rax
  000000014094ADC0: mov         qword ptr [rbp+148h],rcx
  000000014094ADC7: lea         rax,[rcx+20h]
  000000014094ADCB: mov         qword ptr [rbp+8],rax
  000000014094ADCF: movss       xmm13,dword ptr [__real@3a83126f]
  000000014094ADD8: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094ADE1: movss       xmm12,dword ptr [__real@3f800000]
  000000014094ADEA: movss       xmm14,dword ptr [__real@4effffff]
  000000014094ADF3: jmp         000000014094AE00
  000000014094ADF5: subss       xmm0,dword ptr [rdi+28h]
  000000014094ADFA: movss       dword ptr [r14-8],xmm0
  000000014094AE00: test        r15,r15
  000000014094AE03: je          000000014094C010
  000000014094AE09: test        bx,bx
  000000014094AE0C: je          000000014094AE20
  000000014094AE0E: lea         eax,[rbx-1]
  000000014094AE11: and         eax,ebx
  000000014094AE13: jmp         000000014094AE4A
  000000014094AE15: nop         word ptr cs:[rax+rax]
  000000014094AE20: movdqa      xmm0,xmmword ptr [r13]
  000000014094AE26: pmovmskb    ebx,xmm0
  000000014094AE2A: add         r12,0FFFFFFFFFFFFF680h
  000000014094AE31: add         r13,10h
  000000014094AE35: cmp         ebx,0FFFFh
  000000014094AE3B: je          000000014094AE20
  000000014094AE3D: mov         ecx,0FFFFFFFEh
  000000014094AE42: sub         ecx,ebx
  000000014094AE44: not         ebx
  000000014094AE46: mov         eax,ebx
  000000014094AE48: and         eax,ecx
  000000014094AE4A: mov         ecx,ebx
  000000014094AE4C: tzcnt       ecx,ecx
  000000014094AE50: mov         ebx,eax
  000000014094AE52: dec         r15
  000000014094AE55: neg         rcx
  000000014094AE58: imul        rcx,rcx,98h
  000000014094AE5F: lea         r14,[r12+rcx]
  000000014094AE63: cmp         byte ptr [r12+rcx-4],0
  000000014094AE69: je          000000014094AE9B
  000000014094AE6B: mov         rax,qword ptr [rbp+50h]
  000000014094AE6F: mov         rdx,qword ptr [rbp+58h]
  000000014094AE73: mov         r8,qword ptr [r14-30h]
  000000014094AE77: shl         rdx,3
  000000014094AE7B: xor         r9d,r9d
  000000014094AE7E: nop
  000000014094AE80: cmp         rdx,r9
  000000014094AE83: je          000000014094AE00
  000000014094AE89: lea         r10,[r9+8]
  000000014094AE8D: cmp         r8,qword ptr [rax+r9]
  000000014094AE91: mov         r9,r10
  000000014094AE94: jne         000000014094AE80
  000000014094AE96: mov         byte ptr [r14-4],0
  000000014094AE9B: mov         rax,qword ptr [r14-78h]
  000000014094AE9F: cmp         rax,qword ptr [r14-40h]
  000000014094AEA3: jae         000000014094AE00
  000000014094AEA9: movss       xmm0,dword ptr [r14-8]
  000000014094AEAF: ucomiss     xmm0,xmm13
  000000014094AEB3: ja          000000014094ADF5
  000000014094AEB9: add         rcx,r12
  000000014094AEBC: add         rcx,0FFFFFFFFFFFFFF70h
  000000014094AEC3: mov         qword ptr [rbp+1B8h],rcx
  000000014094AECA: cmp         dword ptr [r14-28h],1
  000000014094AECF: jne         000000014094AEF0
  000000014094AED1: cmp         rax,1
  000000014094AED5: ja          000000014094B1BB
  000000014094AEDB: mov         dword ptr [r14-28h],0
  000000014094AEE3: mov         dword ptr [r14-0Ch],0
  000000014094AEEB: jmp         000000014094AE00
  000000014094AEF0: test        rax,rax
  000000014094AEF3: je          000000014094AE00
  000000014094AEF9: mov         rcx,qword ptr [r14-80h]
  000000014094AEFD: add         rax,rcx
  000000014094AF00: dec         rax
  000000014094AF03: mov         rcx,qword ptr [r14-90h]
  000000014094AF0A: mov         rdx,qword ptr [r14-88h]
  000000014094AF11: cmp         rax,rcx
  000000014094AF14: mov         r8d,0
  000000014094AF1A: cmovae      r8,rcx
  000000014094AF1E: sub         rax,r8
  000000014094AF21: lea         rax,[rax+rax*4]
  000000014094AF25: movss       xmm15,dword ptr [rdx+rax*8+8]
  000000014094AF2C: mov         ecx,dword ptr [rdx+rax*8+0Ch]
  000000014094AF30: mov         dword ptr [rbp+198h],ecx
  000000014094AF36: movss       xmm7,dword ptr [rdx+rax*8+10h]
  000000014094AF3C: mov         rax,qword ptr [rbp+190h]
  000000014094AF43: movsd       xmm0,mmword ptr [rax+50h]
  000000014094AF48: movaps      xmm8,xmm0
  000000014094AF4C: mulps       xmm8,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094AF54: movaps      xmm6,xmm15
  000000014094AF58: unpcklps    xmm6,xmm7
  000000014094AF5B: addps       xmm8,xmm6
  000000014094AF5F: movsd       xmm11,mmword ptr [rax+48h]
  000000014094AF65: cvtdq2ps    xmm1,xmm11
  000000014094AF69: divps       xmm0,xmm1
  000000014094AF6C: divps       xmm8,xmm0
  000000014094AF70: movshdup    xmm0,xmm8
  000000014094AF75: call        floorf
  000000014094AF7A: movaps      xmm9,xmm0
  000000014094AF7E: movaps      xmm0,xmm8
  000000014094AF82: call        floorf
  000000014094AF87: cvttss2si   eax,xmm0
  000000014094AF8B: ucomiss     xmm0,xmm14
  000000014094AF8F: mov         r8d,7FFFFFFFh
  000000014094AF95: cmova       eax,r8d
  000000014094AF99: ucomiss     xmm0,xmm0
  000000014094AF9C: mov         edx,0
  000000014094AFA1: cmovp       eax,edx
  000000014094AFA4: cvttss2si   ecx,xmm9
  000000014094AFA9: ucomiss     xmm9,xmm14
  000000014094AFAD: cmova       ecx,r8d
  000000014094AFB1: ucomiss     xmm9,xmm9
  000000014094AFB5: cmovp       ecx,edx
  000000014094AFB8: test        eax,eax
  000000014094AFBA: js          000000014094AE00
  000000014094AFC0: movd        xmm0,eax
  000000014094AFC4: movd        xmm1,ecx
  000000014094AFC8: punpckldq   xmm0,xmm1
  000000014094AFCC: movq        xmm0,xmm0
  000000014094AFD0: movaps      xmm1,xmm11
  000000014094AFD4: pcmpgtd     xmm1,xmm0
  000000014094AFD8: pshufd      xmm1,xmm1,50h
  000000014094AFDD: movmskpd    ecx,xmm1
  000000014094AFE1: test        cl,2
  000000014094AFE4: je          000000014094AE00
  000000014094AFEA: test        cl,1
  000000014094AFED: je          000000014094AE00
  000000014094AFF3: pshufd      xmm0,xmm0,55h
  000000014094AFF8: movd        ecx,xmm0
  000000014094AFFC: test        ecx,ecx
  000000014094AFFE: js          000000014094AE00
  000000014094B004: movd        edx,xmm11
  000000014094B009: imul        ecx,edx
  000000014094B00C: add         ecx,eax
  000000014094B00E: movsxd      rcx,ecx
  000000014094B011: mov         rax,qword ptr [rbp+190h]
  000000014094B018: cmp         qword ptr [rax+10h],rcx
  000000014094B01C: jbe         000000014094AE00
  000000014094B022: mov         rax,qword ptr [rbp+190h]
  000000014094B029: mov         rax,qword ptr [rax+8]
  000000014094B02D: lea         rcx,[rcx+rcx*2]
  000000014094B031: shl         rcx,4
  000000014094B035: mov         rsi,qword ptr [rax+rcx+10h]
  000000014094B03A: test        rsi,rsi
  000000014094B03D: je          000000014094AE00
  000000014094B043: mov         qword ptr [rbp+1B0h],rdi
  000000014094B04A: mov         rdi,qword ptr [rax+rcx+8]
  000000014094B04F: mov         qword ptr [rsp+20h],rsi
  000000014094B054: mov         dword ptr [rsp+28h],3F000000h
  000000014094B05C: lea         rcx,[rbp-40h]
  000000014094B060: movaps      xmm1,xmm15
  000000014094B064: movaps      xmm2,xmm7
  000000014094B067: mov         r9,rdi
  000000014094B06A: call        _ZN12country_core7systems3ivy10ivy_grower21closest_segment_index17hba4d5971b8993167E
  000000014094B06F: cmp         dword ptr [rbp-40h],1
  000000014094B073: jne         000000014094BFB1
  000000014094B079: mov         qword ptr [rbp+130h],rdi
  000000014094B080: movss       xmm0,dword ptr [__real@3ebae148]
  000000014094B088: ucomiss     xmm0,dword ptr [rbp-30h]
  000000014094B08C: mov         rdi,qword ptr [rbp+1B0h]
  000000014094B093: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094B09C: jbe         000000014094AE00
  000000014094B0A2: mov         rcx,qword ptr [rbp-38h]
  000000014094B0A6: cmp         rcx,rsi
  000000014094B0A9: jae         000000014094C237
  000000014094B0AF: imul        rsi,rcx,38h
  000000014094B0B3: mov         rax,qword ptr [rbp+130h]
  000000014094B0BA: mov         rax,qword ptr [rax+rsi+28h]
  000000014094B0BF: mov         qword ptr [rbp+1C0h],rax
  000000014094B0C6: mov         qword ptr [rbp+150h],rax
  000000014094B0CD: mov         rax,qword ptr [rbp+1A0h]
  000000014094B0D4: cmp         qword ptr [rax+18h],0
  000000014094B0D9: je          000000014094AE00
  000000014094B0DF: mov         rcx,qword ptr [rbp+88h]
  000000014094B0E6: lea         rdx,[rbp+150h]
  000000014094B0ED: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  000000014094B0F2: mov         rcx,qword ptr [rbp+130h]
  000000014094B0F9: add         rcx,rsi
  000000014094B0FC: mov         qword ptr [rbp+130h],rcx
  000000014094B103: mov         rdx,qword ptr [rbp+1A0h]
  000000014094B10A: mov         rcx,qword ptr [rdx]
  000000014094B10D: mov         rdx,qword ptr [rdx+8]
  000000014094B111: mov         r8,rdx
  000000014094B114: and         r8,rax
  000000014094B117: shr         rax,39h
  000000014094B11B: movd        xmm0,eax
  000000014094B11F: punpcklbw   xmm0,xmm0
  000000014094B123: pshuflw     xmm0,xmm0,0
  000000014094B128: pshufd      xmm0,xmm0,0
  000000014094B12D: lea         rax,[rcx-2A8h]
  000000014094B134: xor         r9d,r9d
  000000014094B137: movdqu      xmm1,xmmword ptr [rcx+r8]
  000000014094B13D: movdqa      xmm2,xmm1
  000000014094B141: pcmpeqb     xmm2,xmm0
  000000014094B145: pmovmskb    r10d,xmm2
  000000014094B14A: test        r10d,r10d
  000000014094B14D: mov         rdi,qword ptr [rbp+1B0h]
  000000014094B154: je          000000014094B189
  000000014094B156: tzcnt       r11d,r10d
  000000014094B15B: add         r11,r8
  000000014094B15E: and         r11,rdx
  000000014094B161: neg         r11
  000000014094B164: imul        r11,r11,2A8h
  000000014094B16B: mov         rsi,qword ptr [rbp+1C0h]
  000000014094B172: cmp         qword ptr [rax+r11],rsi
  000000014094B176: je          000000014094B31D
  000000014094B17C: lea         r11d,[r10-1]
  000000014094B180: and         r11w,r10w
  000000014094B184: mov         r10d,r11d
  000000014094B187: jne         000000014094B156
  000000014094B189: pcmpeqb     xmm1,xmmword ptr [__xmm@ffffffffffffffffffffffffffffffff]
  000000014094B191: pmovmskb    r10d,xmm1
  000000014094B196: test        r10d,r10d
  000000014094B199: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094B1A2: jne         000000014094AE00
  000000014094B1A8: add         r8,r9
  000000014094B1AB: add         r8,10h
  000000014094B1AF: add         r9,10h
  000000014094B1B3: and         r8,rdx
  000000014094B1B6: jmp         000000014094B137
  000000014094B1BB: movss       xmm0,dword ptr [rdi+28h]
  000000014094B1C0: divss       xmm0,xmm10
  000000014094B1C5: addss       xmm0,dword ptr [r14-0Ch]
  000000014094B1CB: xorps       xmm1,xmm1
  000000014094B1CE: maxss       xmm1,xmm0
  000000014094B1D2: movaps      xmm0,xmm12
  000000014094B1D6: minss       xmm0,xmm1
  000000014094B1DA: movss       dword ptr [r14-0Ch],xmm0
  000000014094B1E0: movaps      xmm1,xmm12
  000000014094B1E4: subss       xmm1,xmm0
  000000014094B1E8: movss       xmm2,dword ptr [r14-1Ch]
  000000014094B1EE: mulss       xmm2,xmm1
  000000014094B1F2: movss       xmm3,dword ptr [r14-10h]
  000000014094B1F8: mulss       xmm3,xmm0
  000000014094B1FC: addss       xmm3,xmm2
  000000014094B200: mov         rcx,qword ptr [r14-80h]
  000000014094B204: add         rax,rcx
  000000014094B207: dec         rax
  000000014094B20A: mov         rcx,qword ptr [r14-90h]
  000000014094B211: mov         rdx,qword ptr [r14-88h]
  000000014094B218: cmp         rax,rcx
  000000014094B21B: mov         r8d,0
  000000014094B221: cmovae      r8,rcx
  000000014094B225: sub         rax,r8
  000000014094B228: lea         rax,[rax+rax*4]
  000000014094B22C: movsd       xmm2,mmword ptr [r14-18h]
  000000014094B232: movsd       xmm4,mmword ptr [r14-24h]
  000000014094B238: movsldup    xmm1,xmm1
  000000014094B23C: mulps       xmm1,xmm4
  000000014094B23F: movsldup    xmm0,xmm0
  000000014094B243: mulps       xmm0,xmm2
  000000014094B246: addps       xmm0,xmm1
  000000014094B249: movlps      qword ptr [rdx+rax*8+8],xmm0
  000000014094B24E: movss       dword ptr [rdx+rax*8+10h],xmm3
  000000014094B254: movss       xmm0,dword ptr [r14-0Ch]
  000000014094B25A: ucomiss     xmm0,dword ptr [__real@3f7d70a4]
  000000014094B261: jbe         000000014094AE00
  000000014094B267: mov         qword ptr [rbp+1B0h],rdi
  000000014094B26E: mov         dword ptr [r14-28h],0
  000000014094B276: mov         dword ptr [r14-0Ch],0
  000000014094B27E: mov         rax,qword ptr [r14-70h]
  000000014094B282: mov         qword ptr [rbp+198h],rax
  000000014094B289: mov         rcx,qword ptr [rbp+1B8h]
  000000014094B290: call        _ZN12country_core9resources3ivy11ivy_storage3Ivy22last_segment_point_ids17h51349857c3f8c7bfE
  000000014094B295: mov         rsi,rax
  000000014094B298: mov         r14,rdx
  000000014094B29B: mov         eax,dword ptr [rbp+0C4h]
  000000014094B2A1: mov         rcx,qword ptr [rbp]
  000000014094B2A5: mov         dword ptr [rcx],eax
  000000014094B2A7: mov         rax,qword ptr [rbp+148h]
  000000014094B2AE: mov         rdi,qword ptr [rax+30h]
  000000014094B2B2: mov         rcx,qword ptr [rax+40h]
  000000014094B2B6: mov         qword ptr [rbp+1B8h],rcx
  000000014094B2BD: cmp         rdi,qword ptr [rax+20h]
  000000014094B2C1: jne         000000014094B2D3
  000000014094B2C3: mov         rcx,qword ptr [rbp+8]
  000000014094B2C7: lea         rdx,[anon.60360aed504c4a0f85b3886feb406ce0.1.llvm.17420070914341975340]
  000000014094B2CE: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h1169a98e92e5dc76E
  000000014094B2D3: mov         rdx,qword ptr [rbp+148h]
  000000014094B2DA: mov         rax,qword ptr [rdx+28h]
  000000014094B2DE: mov         rcx,rdi
  000000014094B2E1: shl         rcx,5
  000000014094B2E5: mov         r8,qword ptr [rbp+1B8h]
  000000014094B2EC: mov         qword ptr [rax+rcx],r8
  000000014094B2F0: mov         qword ptr [rax+rcx+8],rsi
  000000014094B2F5: mov         qword ptr [rax+rcx+10h],r14
  000000014094B2FA: mov         r8,qword ptr [rbp+198h]
  000000014094B301: mov         qword ptr [rax+rcx+18h],r8
  000000014094B306: inc         rdi
  000000014094B309: mov         qword ptr [rdx+30h],rdi
  000000014094B30D: inc         qword ptr [rdx+40h]
  000000014094B311: mov         rdi,qword ptr [rbp+1B0h]
  000000014094B318: jmp         000000014094AE00
  000000014094B31D: cmp         qword ptr [rcx+r11-290h],2
  000000014094B326: jb          000000014094C1E3
  000000014094B32C: movss       xmm0,dword ptr [rcx+r11-270h]
  000000014094B336: ucomiss     xmm0,dword ptr [__real@00000000]
  000000014094B33D: jbe         000000014094C1E3
  000000014094B343: mov         rdi,qword ptr [rbp+1C0h]
  000000014094B34A: mov         qword ptr [rbp+150h],rdi
  000000014094B351: mov         rax,qword ptr [rbp+1A0h]
  000000014094B358: cmp         qword ptr [rax+18h],0
  000000014094B35D: je          000000014094C1C6
  000000014094B363: mov         rcx,qword ptr [rbp+88h]
  000000014094B36A: lea         rdx,[rbp+150h]
  000000014094B371: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  000000014094B376: mov         rcx,qword ptr [rbp+1A0h]
  000000014094B37D: mov         r11,qword ptr [rcx]
  000000014094B380: mov         rcx,qword ptr [rcx+8]
  000000014094B384: mov         rdx,rcx
  000000014094B387: and         rdx,rax
  000000014094B38A: shr         rax,39h
  000000014094B38E: movd        xmm0,eax
  000000014094B392: punpcklbw   xmm0,xmm0
  000000014094B396: pshuflw     xmm0,xmm0,0
  000000014094B39B: pshufd      xmm0,xmm0,0
  000000014094B3A0: lea         rax,[r11-2A8h]
  000000014094B3A7: xor         r8d,r8d
  000000014094B3AA: movdqu      xmm1,xmmword ptr [r11+rdx]
  000000014094B3B0: movdqa      xmm2,xmm1
  000000014094B3B4: pcmpeqb     xmm2,xmm0
  000000014094B3B8: pmovmskb    r9d,xmm2
  000000014094B3BD: test        r9d,r9d
  000000014094B3C0: je          000000014094B3EA
  000000014094B3C2: tzcnt       r10d,r9d
  000000014094B3C7: add         r10,rdx
  000000014094B3CA: and         r10,rcx
  000000014094B3CD: neg         r10
  000000014094B3D0: imul        rsi,r10,2A8h
  000000014094B3D7: cmp         qword ptr [rax+rsi],rdi
  000000014094B3DB: je          000000014094B410
  000000014094B3DD: lea         r10d,[r9-1]
  000000014094B3E1: and         r10w,r9w
  000000014094B3E5: mov         r9d,r10d
  000000014094B3E8: jne         000000014094B3C2
  000000014094B3EA: pcmpeqb     xmm1,xmmword ptr [__xmm@ffffffffffffffffffffffffffffffff]
  000000014094B3F2: pmovmskb    r9d,xmm1
  000000014094B3F7: test        r9d,r9d
  000000014094B3FA: jne         000000014094C1C6
  000000014094B400: add         rdx,r8
  000000014094B403: add         rdx,10h
  000000014094B407: add         r8,10h
  000000014094B40B: and         rdx,rcx
  000000014094B40E: jmp         000000014094B3AA
  000000014094B410: cmp         qword ptr [r11+rsi-290h],2
  000000014094B419: jb          000000014094C200
  000000014094B41F: movss       xmm0,dword ptr [r11+rsi-270h]
  000000014094B429: ucomiss     xmm0,dword ptr [__real@00000000]
  000000014094B430: jbe         000000014094C200
  000000014094B436: lea         rax,[r11+rsi]
  000000014094B43A: cmp         qword ptr [rax-290h],2
  000000014094B442: jb          000000014094C21D
  000000014094B448: movss       xmm0,dword ptr [rax-270h]
  000000014094B450: ucomiss     xmm0,dword ptr [__real@00000000]
  000000014094B457: jbe         000000014094C21D
  000000014094B45D: mov         qword ptr [rbp+1C0h],rax
  000000014094B464: mov         rax,qword ptr [r14-78h]
  000000014094B468: test        rax,rax
  000000014094B46B: je          000000014094C248
  000000014094B471: mov         rdi,r11
  000000014094B474: mov         rcx,qword ptr [r14-30h]
  000000014094B478: mov         qword ptr [rbp+18h],rcx
  000000014094B47C: mov         rcx,qword ptr [r14-80h]
  000000014094B480: add         rax,rcx
  000000014094B483: dec         rax
  000000014094B486: mov         rcx,qword ptr [r14-90h]
  000000014094B48D: cmp         rax,rcx
  000000014094B490: mov         edx,0
  000000014094B495: cmovae      rdx,rcx
  000000014094B499: mov         rcx,qword ptr [r14-88h]
  000000014094B4A0: sub         rax,rdx
  000000014094B4A3: lea         rax,[rax+rax*4]
  000000014094B4A7: mov         edx,dword ptr [rcx+rax*8+10h]
  000000014094B4AB: mov         dword ptr [rbp+158h],edx
  000000014094B4B1: mov         rax,qword ptr [rcx+rax*8+8]
  000000014094B4B6: mov         qword ptr [rbp+150h],rax
  000000014094B4BD: mov         r8,qword ptr [r14-70h]
  000000014094B4C1: mov         rcx,qword ptr [rbp-8]
  000000014094B4C5: lea         rdx,[rbp+150h]
  000000014094B4CC: call        _ZN12country_core9resources3ivy22ivy_direction_proposer20IvyDirectionProposer13get_direction17h3add4d225aa1f4efE
  000000014094B4D1: movaps      xmmword ptr [rbp+20h],xmm0
  000000014094B4D5: movd        dword ptr [rbp+1A8h],xmm1
  000000014094B4DD: mov         rax,qword ptr [r14-78h]
  000000014094B4E1: test        rax,rax
  000000014094B4E4: je          000000014094C256
  000000014094B4EA: add         rsi,rdi
  000000014094B4ED: add         rsi,0FFFFFFFFFFFFFD60h
  000000014094B4F4: mov         rcx,qword ptr [r14-80h]
  000000014094B4F8: add         rax,rcx
  000000014094B4FB: dec         rax
  000000014094B4FE: mov         rcx,qword ptr [r14-90h]
  000000014094B505: mov         rdx,qword ptr [r14-88h]
  000000014094B50C: cmp         rax,rcx
  000000014094B50F: mov         r8d,0
  000000014094B515: cmovae      r8,rcx
  000000014094B519: sub         rax,r8
  000000014094B51C: lea         rax,[rax+rax*4]
  000000014094B520: movss       xmm1,dword ptr [rdx+rax*8+8]
  000000014094B526: movss       xmm2,dword ptr [rdx+rax*8+10h]
  000000014094B52C: mov         rcx,rsi
  000000014094B52F: lea         r9,[142AB3780h]
  000000014094B536: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$21get_approx_u_from_pos17h5da41a19c706643fE
  000000014094B53B: movss       dword ptr [rbp+1ACh],xmm0
  000000014094B543: mov         rax,qword ptr [r14-78h]
  000000014094B547: test        rax,rax
  000000014094B54A: je          000000014094C264
  000000014094B550: mov         rcx,qword ptr [r14-80h]
  000000014094B554: add         rax,rcx
  000000014094B557: dec         rax
  000000014094B55A: mov         rcx,qword ptr [r14-90h]
  000000014094B561: cmp         rax,rcx
  000000014094B564: mov         edx,0
  000000014094B569: cmovae      rdx,rcx
  000000014094B56D: mov         rcx,qword ptr [r14-88h]
  000000014094B574: sub         rax,rdx
  000000014094B577: lea         rax,[rax+rax*4]
  000000014094B57B: movss       xmm8,dword ptr [rcx+rax*8+8]
  000000014094B582: movss       xmm0,dword ptr [rcx+rax*8+0Ch]
  000000014094B588: movss       dword ptr [rbp+60h],xmm0
  000000014094B58D: mov         rdx,qword ptr [rbp+1C0h]
  000000014094B594: movss       xmm9,dword ptr [rdx-270h]
  000000014094B59D: movss       xmm1,dword ptr [rbp+1ACh]
  000000014094B5A5: mulss       xmm9,xmm1
  000000014094B5AA: xorps       xmm0,xmm0
  000000014094B5AD: maxss       xmm0,xmm1
  000000014094B5B1: movaps      xmm11,xmm12
  000000014094B5B5: minss       xmm11,xmm0
  000000014094B5BA: movss       xmm0,dword ptr [rcx+rax*8+10h]
  000000014094B5C0: movaps      xmmword ptr [rbp+0E0h],xmm0
  000000014094B5C7: mov         rcx,rsi
  000000014094B5CA: movaps      xmm1,xmm11
  000000014094B5CE: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094B5D3: mov         eax,eax
  000000014094B5D5: lea         rcx,[rax+1]
  000000014094B5D9: mov         r8,qword ptr [rbp+1C0h]
  000000014094B5E0: mov         rdx,qword ptr [r8-290h]
  000000014094B5E7: cmp         rcx,rdx
  000000014094B5EA: jae         000000014094C272
  000000014094B5F0: mov         rcx,qword ptr [r8-298h]
  000000014094B5F7: movsd       xmm0,mmword ptr [rcx+rax*8]
  000000014094B5FC: movaps      xmmword ptr [rbp+30h],xmm0
  000000014094B600: movsd       xmm10,mmword ptr [rcx+rax*8+8]
  000000014094B607: mov         rcx,rsi
  000000014094B60A: movaps      xmm1,xmm11
  000000014094B60E: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094B613: mov         qword ptr [rbp+90h],rsi
  000000014094B61A: mov         ecx,eax
  000000014094B61C: mov         rax,qword ptr [rbp+1C0h]
  000000014094B623: mov         rdx,qword ptr [rax-290h]
  000000014094B62A: cmp         rdx,rcx
  000000014094B62D: jbe         000000014094C280
  000000014094B633: lea         rax,[rcx+1]
  000000014094B637: cmp         rax,rdx
  000000014094B63A: mov         rdi,qword ptr [rbp+1B0h]
  000000014094B641: movss       xmm5,dword ptr [rbp+1A8h]
  000000014094B649: jae         000000014094C28E
  000000014094B64F: movss       xmm1,dword ptr [__real@3e570a3e]
  000000014094B657: movaps      xmm11,xmmword ptr [rbp+20h]
  000000014094B65C: mulss       xmm11,xmm1
  000000014094B661: mulss       xmm5,xmm1
  000000014094B665: addss       xmm5,dword ptr [rbp+60h]
  000000014094B66A: subps       xmm10,xmmword ptr [rbp+30h]
  000000014094B66F: movaps      xmm1,xmm10
  000000014094B673: mulps       xmm1,xmm10
  000000014094B677: movshdup    xmm2,xmm1
  000000014094B67B: addss       xmm2,xmm1
  000000014094B67F: xorps       xmm1,xmm1
  000000014094B682: sqrtss      xmm1,xmm2
  000000014094B686: movaps      xmm2,xmm12
  000000014094B68A: divss       xmm2,xmm1
  000000014094B68E: movsldup    xmm1,xmm2
  000000014094B692: mulps       xmm1,xmm10
  000000014094B696: mov         rdx,qword ptr [rbp+1C0h]
  000000014094B69D: mov         rax,qword ptr [rdx-298h]
  000000014094B6A4: movaps      xmm2,xmm12
  000000014094B6A8: subss       xmm2,xmm0
  000000014094B6AC: movsd       xmm3,mmword ptr [rax+rcx*8]
  000000014094B6B1: movsd       xmm4,mmword ptr [rax+rcx*8+8]
  000000014094B6B7: movsldup    xmm2,xmm2
  000000014094B6BB: mulps       xmm2,xmm3
  000000014094B6BE: movsldup    xmm0,xmm0
  000000014094B6C2: mulps       xmm0,xmm4
  000000014094B6C5: addps       xmm0,xmm2
  000000014094B6C8: unpcklps    xmm8,xmmword ptr [rbp+0E0h]
  000000014094B6D0: shufps      xmm0,xmm0,0E1h
  000000014094B6D4: shufps      xmm8,xmm8,0E1h
  000000014094B6D9: subps       xmm8,xmm0
  000000014094B6DD: mulps       xmm8,xmm1
  000000014094B6E1: movshdup    xmm0,xmm8
  000000014094B6E6: movaps      xmm1,xmm11
  000000014094B6EA: xorps       xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094B6F1: cmpltss     xmm8,xmm0
  000000014094B6F7: andps       xmm1,xmm8
  000000014094B6FB: andnps      xmm8,xmm11
  000000014094B6FF: orps        xmm8,xmm1
  000000014094B703: addss       xmm9,xmm8
  000000014094B708: movss       dword ptr [rbp+120h],xmm9
  000000014094B711: movss       dword ptr [rbp+124h],xmm5
  000000014094B719: xorps       xmm0,xmm0
  000000014094B71C: ucomiss     xmm0,xmm9
  000000014094B720: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094B729: ja          000000014094AE00
  000000014094B72F: ucomiss     xmm9,dword ptr [rdx-270h]
  000000014094B737: ja          000000014094AE00
  000000014094B73D: cmp         byte ptr [rdx-0Ah],0
  000000014094B741: je          000000014094B752
  000000014094B743: movss       xmm0,dword ptr [rdx-1Ch]
  000000014094B748: addss       xmm0,dword ptr [__real@3e4ccccd]
  000000014094B750: jmp         000000014094B777
  000000014094B752: cmp         dword ptr [rdx-48h],2
  000000014094B756: movss       xmm0,dword ptr [rdx-1Ch]
  000000014094B75B: jae         000000014094B76F
  000000014094B75D: addss       xmm0,dword ptr [__real@bf4a3d71]
  000000014094B765: addss       xmm0,dword ptr [__real@bdcccccd]
  000000014094B76D: jmp         000000014094B777
  000000014094B76F: addss       xmm0,dword ptr [__real@beeb851f]
  000000014094B777: ucomiss     xmm5,xmm0
  000000014094B77A: ja          000000014094AE00
  000000014094B780: mov         rcx,qword ptr [rbp+1B8h]
  000000014094B787: call        _ZN12country_core9resources3ivy11ivy_storage3Ivy10last_point17hb11cca6d1038791dE
  000000014094B78C: mov         rsi,qword ptr [rbp+90h]
  000000014094B793: movss       xmm1,dword ptr [rax+8]
  000000014094B798: movss       xmm2,dword ptr [rax+10h]
  000000014094B79D: mov         rcx,rsi
  000000014094B7A0: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$16is_on_right_side17h9b4fcfa193d90f90E
  000000014094B7A5: lea         rcx,[rbp+108h]
  000000014094B7AC: lea         rdx,[rbp+120h]
  000000014094B7B3: mov         r8,rsi
  000000014094B7B6: mov         r9d,eax
  000000014094B7B9: call        _ZN12country_core9resources3ivy10wall_coord9WallCoord16into_world_space17h40661c77cf2d7316E
  000000014094B7BE: mov         rcx,qword ptr [rbp+10h]
  000000014094B7C2: mov         rdx,qword ptr [rbp+18h]
  000000014094B7C6: call        _ZN12country_core9resources5walls10wall_holes13PrevWallHoles10wall_holes17ha41cb6deda6effb4E
  000000014094B7CB: mov         rsi,rax
  000000014094B7CE: test        rax,rax
  000000014094B7D1: je          000000014094B873
  000000014094B7D7: cmp         rdx,rsi
  000000014094B7DA: je          000000014094B873
  000000014094B7E0: mov         rax,qword ptr [rbp+1C0h]
  000000014094B7E7: movss       xmm8,dword ptr [rax-14h]
  000000014094B7ED: mov         rcx,qword ptr [rbp+1B8h]
  000000014094B7F4: mov         rdi,rdx
  000000014094B7F7: call        _ZN12country_core9resources3ivy11ivy_storage3Ivy10last_point17hb11cca6d1038791dE
  000000014094B7FC: movss       xmm0,dword ptr [rbp+1ACh]
  000000014094B804: mulss       xmm0,xmm8
  000000014094B809: movss       xmm1,dword ptr [rax+0Ch]
  000000014094B80E: call        _ZN101_$LT$$LP$$RP$$u20$as$u20$country_core..resources..walls..decorator_storage..DecoAddrChangeTracker$GT$6insert17h37bcd7d6ada1fd09E
  000000014094B813: mov         rcx,rdi
  000000014094B816: movaps      xmm8,xmm0
  000000014094B81A: movaps      xmm9,xmm1
  000000014094B81E: jmp         000000014094B829
  000000014094B820: add         rsi,14h
  000000014094B824: cmp         rsi,rcx
  000000014094B827: je          000000014094B873
  000000014094B829: cmp         byte ptr [rsi+10h],0
  000000014094B82D: jne         000000014094B820
  000000014094B82F: movups      xmm0,xmmword ptr [rsi]
  000000014094B832: movaps      xmmword ptr [rbp+150h],xmm0
  000000014094B839: movaps      xmm0,xmmword ptr [rbp+150h]
  000000014094B840: addps       xmm0,xmmword ptr [__xmm@3f8000003dcccccdbf800000bdcccccd]
  000000014094B847: movaps      xmmword ptr [rbp+150h],xmm0
  000000014094B84E: lea         rcx,[rbp+150h]
  000000014094B855: movaps      xmm1,xmm8
  000000014094B859: movaps      xmm2,xmm9
  000000014094B85D: call        _ZN5utils8geometry4aabb5Aabb28contains17h257c0faa2f9f21ebE
  000000014094B862: mov         rcx,rdi
  000000014094B865: test        al,al
  000000014094B867: je          000000014094B820
  000000014094B869: mov         byte ptr [r14-4],1
  000000014094B86E: jmp         000000014094BFB1
  000000014094B873: mov         ecx,dword ptr [rbp+108h]
  000000014094B879: movss       xmm8,dword ptr [rbp+10Ch]
  000000014094B882: mov         edx,dword ptr [rbp+110h]
  000000014094B888: mov         dword ptr [rbp+0FCh],ecx
  000000014094B88E: movss       dword ptr [rbp+100h],xmm8
  000000014094B897: mov         dword ptr [rbp+104h],edx
  000000014094B89D: movss       dword ptr [rbp+0B8h],xmm15
  000000014094B8A6: mov         eax,dword ptr [rbp+198h]
  000000014094B8AC: mov         dword ptr [rbp+0BCh],eax
  000000014094B8B2: movss       dword ptr [rbp+0C0h],xmm7
  000000014094B8BA: mov         qword ptr [rbp+0D8h],0
  000000014094B8C5: mov         dword ptr [rbp+1ACh],ecx
  000000014094B8CB: movd        xmm3,ecx
  000000014094B8CF: lea         rax,[rbp+0C8h]
  000000014094B8D6: mov         qword ptr [rsp+28h],rax
  000000014094B8DB: mov         dword ptr [rbp+1A8h],edx
  000000014094B8E1: mov         dword ptr [rsp+20h],edx
  000000014094B8E5: mov         rcx,qword ptr [rbp+190h]
  000000014094B8EC: movaps      xmm1,xmm15
  000000014094B8F0: movaps      xmm2,xmm7
  000000014094B8F3: call        _ZN12country_core7systems4wall10wall_state17wall_spatial_hash15WallSpatialHash18query_with_segment17h2b2ebbde8a9ebf43E
  000000014094B8F8: lea         rcx,[rbp+0C8h]
  000000014094B8FF: mov         rdx,qword ptr [rbp+130h]
  000000014094B906: call        _ZN5alloc3vec16Vec$LT$T$C$A$GT$6retain17hdecd0aa588ed1dddE
  000000014094B90B: mov         rax,qword ptr [rbp+0D0h]
  000000014094B912: mov         rcx,qword ptr [rbp+0D8h]
  000000014094B919: lea         rcx,[rax+rcx*8]
  000000014094B91D: mov         qword ptr [rbp+150h],rax
  000000014094B924: mov         qword ptr [rbp+158h],rcx
  000000014094B92B: lea         rax,[rbp+0B8h]
  000000014094B932: mov         qword ptr [rbp+160h],rax
  000000014094B939: lea         rax,[rbp+0FCh]
  000000014094B940: mov         qword ptr [rbp+168h],rax
  000000014094B947: lea         rcx,[rbp+108h]
  000000014094B94E: lea         rdx,[rbp+150h]
  000000014094B955: lea         r8,[anon.a9f01d7c42a6b6ded4ee4033507219c5.7.llvm.1057620027058656075]
  000000014094B95C: call        _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17hb6f3aedbf995e303E
  000000014094B961: movss       dword ptr [rbp+20h],xmm8
  000000014094B967: mov         rdx,qword ptr [rbp+118h]
  000000014094B96E: test        rdx,rdx
  000000014094B971: mov         rdi,qword ptr [rbp+1B0h]
  000000014094B978: je          000000014094BD7D
  000000014094B97E: mov         rax,qword ptr [rbp+110h]
  000000014094B985: mov         qword ptr [rbp+0E0h],rax
  000000014094B98C: lea         rax,[rbp+9Ch]
  000000014094B993: mov         qword ptr [rbp+150h],rax
  000000014094B99A: cmp         rdx,1
  000000014094B99E: jne         000000014094BFC6
  000000014094B9A4: mov         rax,qword ptr [rbp+0E0h]
  000000014094B9AB: movsd       xmm0,mmword ptr [rax]
  000000014094B9AF: movsd       mmword ptr [rbp+80h],xmm0
  000000014094B9B7: mov         rdx,qword ptr [rax+10h]
  000000014094B9BB: mov         rcx,qword ptr [rbp+1A0h]
  000000014094B9C2: lea         r8,[142AB3798h]
  000000014094B9C9: call        _ZN12country_core9resources5walls12public_walls11PublicWalls3get17hf9b6b2a0178344aeE
  000000014094B9CE: mov         qword ptr [rbp+1C0h],rax
  000000014094B9D5: mov         rax,qword ptr [rbp+130h]
  000000014094B9DC: mov         rdx,qword ptr [rax+28h]
  000000014094B9E0: mov         rcx,qword ptr [rbp+1A0h]
  000000014094B9E7: lea         r8,[142AB37B0h]
  000000014094B9EE: call        _ZN12country_core9resources5walls12public_walls11PublicWalls3get17hf9b6b2a0178344aeE
  000000014094B9F3: cmp         qword ptr [rbp+1C0h],0
  000000014094B9FB: je          000000014094BD7D
  000000014094BA01: mov         rsi,rax
  000000014094BA04: test        rax,rax
  000000014094BA07: je          000000014094BD7D
  000000014094BA0D: lea         rcx,[rbp+9Ch]
  000000014094BA14: lea         rdx,[rbp+80h]
  000000014094BA1B: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094BA20: movss       xmm0,dword ptr [rbp+100h]
  000000014094BA28: movaps      xmmword ptr [rbp+60h],xmm0
  000000014094BA2C: movss       dword ptr [rbp+0A0h],xmm0
  000000014094BA34: movss       xmm1,dword ptr [rbp+9Ch]
  000000014094BA3C: movss       xmm2,dword ptr [rbp+0A4h]
  000000014094BA44: mov         rcx,qword ptr [rbp+1C0h]
  000000014094BA4B: movaps      xmmword ptr [rbp+130h],xmm1
  000000014094BA52: movaps      xmm10,xmm2
  000000014094BA56: lea         r9,[142AB37C8h]
  000000014094BA5D: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$21get_approx_u_from_pos17h5da41a19c706643fE
  000000014094BA62: xorps       xmm1,xmm1
  000000014094BA65: maxss       xmm1,xmm0
  000000014094BA69: movaps      xmm9,xmm12
  000000014094BA6D: minss       xmm9,xmm1
  000000014094BA72: mov         rcx,qword ptr [rbp+1C0h]
  000000014094BA79: movaps      xmm1,xmm9
  000000014094BA7D: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$16get_tangent_at_u17h9d04477ed24e5a61E
  000000014094BA82: movaps      xmm8,xmm0
  000000014094BA86: movaps      xmm11,xmm1
  000000014094BA8A: movaps      xmm0,xmm1
  000000014094BA8D: xorps       xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094BA94: movss       dword ptr [rbp+188h],xmm0
  000000014094BA9C: movss       dword ptr [rbp+18Ch],xmm8
  000000014094BAA5: mov         rcx,qword ptr [rbp+1C0h]
  000000014094BAAC: movaps      xmm1,xmm9
  000000014094BAB0: lea         r8,[142AB37E0h]
  000000014094BAB7: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$12get_pos_at_u17haf8394364de6a3c0E
  000000014094BABC: movaps      xmm2,xmmword ptr [rbp+130h]
  000000014094BAC3: subss       xmm2,xmm0
  000000014094BAC7: movaps      xmm0,xmm10
  000000014094BACB: subss       xmm0,xmm1
  000000014094BACF: mulss       xmm8,xmm0
  000000014094BAD4: mulss       xmm11,xmm2
  000000014094BAD9: ucomiss     xmm11,xmm8
  000000014094BADD: jbe         000000014094BB20
  000000014094BADF: lea         rcx,[rbp+150h]
  000000014094BAE6: lea         rdx,[rbp+188h]
  000000014094BAED: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094BAF2: movss       xmm0,dword ptr [rbp+158h]
  000000014094BAFA: movaps      xmm2,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094BB01: xorps       xmm0,xmm2
  000000014094BB04: movsd       xmm1,mmword ptr [rbp+150h]
  000000014094BB0C: xorps       xmm1,xmm2
  000000014094BB0F: movlps      qword ptr [rbp+120h],xmm1
  000000014094BB16: movss       dword ptr [rbp+128h],xmm0
  000000014094BB1E: jmp         000000014094BB33
  000000014094BB20: lea         rcx,[rbp+120h]
  000000014094BB27: lea         rdx,[rbp+188h]
  000000014094BB2E: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094BB33: mov         rcx,rsi
  000000014094BB36: movaps      xmm1,xmmword ptr [rbp+130h]
  000000014094BB3D: movaps      xmm2,xmm10
  000000014094BB41: call        _ZN5utils5curve41Curve$LT$glam..f32..vec2..Vec2$C$CSem$GT$13project_point17ha185fa8eb66074adE
  000000014094BB46: movss       dword ptr [rbp+188h],xmm0
  000000014094BB4E: movss       dword ptr [rbp+18Ch],xmm1
  000000014094BB56: lea         rcx,[rbp+150h]
  000000014094BB5D: lea         rdx,[rbp+188h]
  000000014094BB64: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094BB69: movss       xmm0,dword ptr [rbp+158h]
  000000014094BB71: movaps      xmmword ptr [rbp+30h],xmm10
  000000014094BB76: subss       xmm0,xmm10
  000000014094BB7B: movaps      xmm1,xmm0
  000000014094BB7E: mulss       xmm1,xmm0
  000000014094BB82: movss       xmm2,dword ptr [rbp+150h]
  000000014094BB8A: movaps      xmm3,xmmword ptr [rbp+60h]
  000000014094BB8E: movaps      xmm5,xmm3
  000000014094BB91: unpcklps    xmm5,xmm2
  000000014094BB94: movaps      xmm2,xmm3
  000000014094BB97: unpcklps    xmm2,xmmword ptr [rbp+130h]
  000000014094BB9E: subps       xmm5,xmm2
  000000014094BBA1: movaps      xmm2,xmm5
  000000014094BBA4: mulps       xmm2,xmm5
  000000014094BBA7: movshdup    xmm3,xmm2
  000000014094BBAB: addss       xmm3,xmm2
  000000014094BBAF: addss       xmm3,xmm1
  000000014094BBB3: xorps       xmm1,xmm1
  000000014094BBB6: sqrtss      xmm1,xmm3
  000000014094BBBA: movaps      xmm3,xmm12
  000000014094BBBE: divss       xmm3,xmm1
  000000014094BBC2: movsldup    xmm4,xmm3
  000000014094BBC6: mulps       xmm4,xmm5
  000000014094BBC9: mulss       xmm3,xmm0
  000000014094BBCD: movss       xmm1,dword ptr [rbp+128h]
  000000014094BBD5: movss       xmm0,dword ptr [rbp+0FCh]
  000000014094BBDD: movss       xmm2,dword ptr [rbp+104h]
  000000014094BBE5: unpcklps    xmm0,xmm2
  000000014094BBE8: movsd       xmm5,mmword ptr [rbp+120h]
  000000014094BBF0: movaps      xmm2,xmm5
  000000014094BBF3: movshdup    xmm8,xmm5
  000000014094BBF8: movaps      xmm9,xmm1
  000000014094BBFC: movlhps     xmm9,xmm5
  000000014094BC00: shufps      xmm9,xmm5,0E2h
  000000014094BC05: movaps      xmm10,xmm1
  000000014094BC09: movaps      xmm11,xmm5
  000000014094BC0D: movss       xmm5,xmm1
  000000014094BC11: mulss       xmm1,xmm4
  000000014094BC15: mulss       xmm8,xmm3
  000000014094BC1A: subss       xmm8,xmm1
  000000014094BC1F: movss       xmm1,dword ptr [rbp+0B8h]
  000000014094BC27: mulps       xmm9,xmm4
  000000014094BC2B: shufps      xmm3,xmm4,0D4h
  000000014094BC2F: shufps      xmm3,xmm4,0F2h
  000000014094BC33: movsd       xmm4,mmword ptr [rbp+80h]
  000000014094BC3B: shufps      xmm2,xmm2,0E1h
  000000014094BC3F: mulps       xmm3,xmm2
  000000014094BC42: subps       xmm9,xmm3
  000000014094BC46: mulss       xmm10,xmm8
  000000014094BC4B: mulss       xmm11,xmm9
  000000014094BC50: subss       xmm10,xmm11
  000000014094BC55: movaps      xmm11,xmmword ptr [rbp+60h]
  000000014094BC5A: mulps       xmm2,xmm9
  000000014094BC5E: shufps      xmm8,xmm9,0D4h
  000000014094BC63: shufps      xmm8,xmm9,0F2h
  000000014094BC68: mulps       xmm8,xmm5
  000000014094BC6C: subps       xmm2,xmm8
  000000014094BC70: mulss       xmm10,xmm10
  000000014094BC75: movaps      xmm3,xmm2
  000000014094BC78: mulps       xmm3,xmm2
  000000014094BC7B: addss       xmm10,xmm3
  000000014094BC80: movshdup    xmm3,xmm3
  000000014094BC84: addss       xmm3,xmm10
  000000014094BC89: sqrtss      xmm3,xmm3
  000000014094BC8D: movaps      xmm5,xmm12
  000000014094BC91: divss       xmm5,xmm3
  000000014094BC95: movsldup    xmm3,xmm5
  000000014094BC99: mulps       xmm3,xmm2
  000000014094BC9C: movaps      xmm10,xmmword ptr [rbp+130h]
  000000014094BCA4: unpcklps    xmm10,xmmword ptr [rbp+30h]
  000000014094BCA9: movsd       xmm2,mmword ptr [rbp+0BCh]
  000000014094BCB1: movaps      xmm5,xmm2
  000000014094BCB4: movss       xmm5,xmm1
  000000014094BCB8: subps       xmm0,xmm5
  000000014094BCBB: mulps       xmm0,xmm0
  000000014094BCBE: movshdup    xmm8,xmm0
  000000014094BCC3: addss       xmm8,xmm0
  000000014094BCC8: xorps       xmm0,xmm0
  000000014094BCCB: sqrtss      xmm0,xmm8
  000000014094BCD0: subps       xmm4,xmm5
  000000014094BCD3: mulps       xmm4,xmm4
  000000014094BCD6: movshdup    xmm5,xmm4
  000000014094BCDA: addss       xmm5,xmm4
  000000014094BCDE: xorps       xmm4,xmm4
  000000014094BCE1: sqrtss      xmm4,xmm5
  000000014094BCE5: divss       xmm4,xmm0
  000000014094BCE9: movaps      xmm5,xmm12
  000000014094BCED: subss       xmm5,xmm4
  000000014094BCF1: maxss       xmm5,dword ptr [__real@00000000]
  000000014094BCF9: mulss       xmm5,xmm0
  000000014094BCFD: movsldup    xmm9,xmm5
  000000014094BD02: mulps       xmm9,xmm3
  000000014094BD06: addps       xmm9,xmm10
  000000014094BD0A: movaps      xmm0,xmm9
  000000014094BD0E: subss       xmm0,xmm1
  000000014094BD12: movaps      xmm1,xmm9
  000000014094BD16: movss       xmm1,xmm11
  000000014094BD1B: subps       xmm1,xmm2
  000000014094BD1E: mulss       xmm0,xmm0
  000000014094BD22: mulps       xmm1,xmm1
  000000014094BD25: addss       xmm0,xmm1
  000000014094BD29: movshdup    xmm1,xmm1
  000000014094BD2D: addss       xmm1,xmm0
  000000014094BD31: xorps       xmm0,xmm0
  000000014094BD34: sqrtss      xmm0,xmm1
  000000014094BD38: movss       xmm1,dword ptr [__real@3ed70a3e]
  000000014094BD40: ucomiss     xmm1,xmm0
  000000014094BD43: jbe         000000014094BD7D
  000000014094BD45: mov         rax,qword ptr [rbp+108h]
  000000014094BD4C: test        rax,rax
  000000014094BD4F: je          000000014094BD6B
  000000014094BD51: shl         rax,3
  000000014094BD55: lea         rdx,[rax+rax*2]
  000000014094BD59: mov         r8d,8
  000000014094BD5F: mov         rcx,qword ptr [rbp+0E0h]
  000000014094BD66: call        __rust_dealloc
  000000014094BD6B: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094BD74: mov         rsi,qword ptr [rbp+1C0h]
  000000014094BD7B: jmp         000000014094BDCE
  000000014094BD7D: mov         rax,qword ptr [rbp+108h]
  000000014094BD84: test        rax,rax
  000000014094BD87: movss       xmm11,dword ptr [rbp+20h]
  000000014094BD8D: je          000000014094BDA9
  000000014094BD8F: mov         rcx,qword ptr [rbp+110h]
  000000014094BD96: shl         rax,3
  000000014094BD9A: lea         rdx,[rax+rax*2]
  000000014094BD9E: mov         r8d,8
  000000014094BDA4: call        __rust_dealloc
  000000014094BDA9: movss       xmm0,dword ptr [rbp+1A8h]
  000000014094BDB1: movss       xmm9,dword ptr [rbp+1ACh]
  000000014094BDBA: unpcklps    xmm9,xmm0
  000000014094BDBE: mov         rsi,qword ptr [rbp+90h]
  000000014094BDC5: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094BDCE: cmp         qword ptr [rsi+10h],2
  000000014094BDD3: jb          000000014094AE00
  000000014094BDD9: pshufd      xmm2,xmm9,0F5h
  000000014094BDDF: mov         rcx,rsi
  000000014094BDE2: movaps      xmm1,xmm9
  000000014094BDE6: lea         r9,[142AB3768h]
  000000014094BDED: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$21get_approx_u_from_pos17h5da41a19c706643fE
  000000014094BDF2: ucomiss     xmm12,xmm0
  000000014094BDF6: mov         rdi,qword ptr [rbp+1B0h]
  000000014094BDFD: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094BE06: jb          000000014094AE00
  000000014094BE0C: ucomiss     xmm0,dword ptr [__real@00000000]
  000000014094BE13: jb          000000014094AE00
  000000014094BE19: cmp         byte ptr [rsi+296h],0
  000000014094BE20: je          000000014094BE34
  000000014094BE22: movss       xmm1,dword ptr [rsi+284h]
  000000014094BE2A: addss       xmm1,dword ptr [__real@3e4ccccd]
  000000014094BE32: jmp         000000014094BE5F
  000000014094BE34: cmp         dword ptr [rsi+258h],2
  000000014094BE3B: movss       xmm1,dword ptr [rsi+284h]
  000000014094BE43: jae         000000014094BE57
  000000014094BE45: addss       xmm1,dword ptr [__real@bf4a3d71]
  000000014094BE4D: addss       xmm1,dword ptr [__real@bdcccccd]
  000000014094BE55: jmp         000000014094BE5F
  000000014094BE57: addss       xmm1,dword ptr [__real@beeb851f]
  000000014094BE5F: ucomiss     xmm1,xmm11
  000000014094BE63: jbe         000000014094AE00
  000000014094BE69: xorps       xmm1,xmm1
  000000014094BE6C: maxss       xmm1,xmm0
  000000014094BE70: movaps      xmm2,xmm12
  000000014094BE74: minss       xmm2,xmm1
  000000014094BE78: movd        dword ptr [rbp+150h],xmm9
  000000014094BE81: movss       dword ptr [rbp+154h],xmm11
  000000014094BE8A: pshufd      xmm8,xmm9,55h
  000000014094BE90: movd        dword ptr [rbp+158h],xmm8
  000000014094BE99: lea         rcx,[rbp+108h]
  000000014094BEA0: mov         rdx,rsi
  000000014094BEA3: lea         r9,[rbp+150h]
  000000014094BEAA: call        _ZN12country_core7systems3ivy10ivy_grower28get_wall_double_sided_normal17h58918def05de80b9E
  000000014094BEAF: movss       xmm0,dword ptr [rbp+110h]
  000000014094BEB7: movaps      xmm1,xmm0
  000000014094BEBA: mulss       xmm1,xmm0
  000000014094BEBE: movsd       xmm2,mmword ptr [rbp+108h]
  000000014094BEC6: movaps      xmm3,xmm2
  000000014094BEC9: mulps       xmm3,xmm2
  000000014094BECC: movshdup    xmm4,xmm3
  000000014094BED0: addss       xmm4,xmm3
  000000014094BED4: addss       xmm4,xmm1
  000000014094BED8: xorps       xmm1,xmm1
  000000014094BEDB: sqrtss      xmm1,xmm4
  000000014094BEDF: movaps      xmm3,xmm12
  000000014094BEE3: divss       xmm3,xmm1
  000000014094BEE7: movsldup    xmm1,xmm3
  000000014094BEEB: mulps       xmm1,xmm2
  000000014094BEEE: mulss       xmm3,xmm0
  000000014094BEF2: movlps      qword ptr [rbp-18h],xmm1
  000000014094BEF6: movss       dword ptr [rbp-10h],xmm3
  000000014094BEFB: movss       xmm0,dword ptr [rbp+198h]
  000000014094BF03: subps       xmm6,xmm9
  000000014094BF07: subss       xmm0,xmm11
  000000014094BF0C: mulss       xmm0,xmm0
  000000014094BF10: mulps       xmm6,xmm6
  000000014094BF13: addss       xmm0,xmm6
  000000014094BF17: movshdup    xmm1,xmm6
  000000014094BF1B: addss       xmm1,xmm0
  000000014094BF1F: xorps       xmm0,xmm0
  000000014094BF22: sqrtss      xmm0,xmm1
  000000014094BF26: movss       xmm1,dword ptr [__real@3ed70a3e]
  000000014094BF2E: ucomiss     xmm1,xmm0
  000000014094BF31: mov         rdi,qword ptr [rbp+1B0h]
  000000014094BF38: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094BF41: jbe         000000014094AE00
  000000014094BF47: movss       dword ptr [rbp+150h],xmm15
  000000014094BF50: mov         eax,dword ptr [rbp+198h]
  000000014094BF56: mov         dword ptr [rbp+154h],eax
  000000014094BF5C: movss       dword ptr [rbp+158h],xmm7
  000000014094BF64: mov         rcx,qword ptr [rbp+1B8h]
  000000014094BF6B: lea         rdx,[rbp+150h]
  000000014094BF72: lea         r8,[rbp-18h]
  000000014094BF76: call        _ZN12country_core9resources3ivy11ivy_storage3Ivy9add_point17hf13199755276e8f2E
  000000014094BF7B: movd        eax,xmm9
  000000014094BF80: movd        ecx,xmm8
  000000014094BF85: mov         dword ptr [r14-28h],1
  000000014094BF8D: movss       dword ptr [r14-24h],xmm15
  000000014094BF93: mov         edx,dword ptr [rbp+198h]
  000000014094BF99: mov         dword ptr [r14-20h],edx
  000000014094BF9D: movss       dword ptr [r14-1Ch],xmm7
  000000014094BFA3: mov         dword ptr [r14-18h],eax
  000000014094BFA7: movss       dword ptr [r14-14h],xmm11
  000000014094BFAD: mov         dword ptr [r14-10h],ecx
  000000014094BFB1: mov         rdi,qword ptr [rbp+1B0h]
  000000014094BFB8: movss       xmm10,dword ptr [__real@3d99999a]
  000000014094BFC1: jmp         000000014094AE00
  000000014094BFC6: cmp         rdx,15h
  000000014094BFCA: jae         000000014094BFF1
  000000014094BFCC: mov         r8d,1
  000000014094BFD2: mov         rcx,qword ptr [rbp+0E0h]
  000000014094BFD9: lea         r9,[rbp+150h]
  000000014094BFE0: call        _ZN4core5slice4sort6shared9smallsort25insertion_sort_shift_left17h7cb52b2d82bd38fbE
  000000014094BFE5: mov         rdi,qword ptr [rbp+1B0h]
  000000014094BFEC: jmp         000000014094B9A4
  000000014094BFF1: mov         rcx,qword ptr [rbp+0E0h]
  000000014094BFF8: lea         r8,[rbp+150h]
  000000014094BFFF: call        _ZN4core5slice4sort6stable14driftsort_main17h3147313f1f45612dE
  000000014094C004: mov         rdi,qword ptr [rbp+1B0h]
  000000014094C00B: jmp         000000014094B9A4
  000000014094C010: mov         rdx,qword ptr [rbp+0C8h]
  000000014094C017: test        rdx,rdx
  000000014094C01A: je          000000014094C032
  000000014094C01C: mov         rcx,qword ptr [rbp+0D0h]
  000000014094C023: shl         rdx,3
  000000014094C027: mov         r8d,8
  000000014094C02D: call        __rust_dealloc
  000000014094C032: mov         rdx,qword ptr [rbp+48h]
  000000014094C036: test        rdx,rdx
  000000014094C039: je          000000014094C04E
  000000014094C03B: mov         rcx,qword ptr [rbp+50h]
  000000014094C03F: shl         rdx,3
  000000014094C043: mov         r8d,8
  000000014094C049: call        __rust_dealloc
  000000014094C04E: cmp         qword ptr [rbp+0A8h],0
  000000014094C056: je          000000014094C10C
  000000014094C05C: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  000000014094C061: mov         rcx,rax
  000000014094C064: mov         rax,qword ptr [rax]
  000000014094C067: cmp         rax,1
  000000014094C06B: jne         000000014094C188
  000000014094C071: add         rcx,8
  000000014094C075: cmp         qword ptr [rcx],0
  000000014094C079: jne         000000014094C1D7
  000000014094C07F: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  000000014094C086: mov         qword ptr [rbp+1B8h],rcx
  000000014094C08D: add         rcx,8
  000000014094C091: mov         rdx,qword ptr [rbp+0B0h]
  000000014094C098: call        _ZN6puffin14ThreadProfiler9end_scope17hbdd5f34d320e5739E
  000000014094C09D: jmp         000000014094C102
  000000014094C09F: mov         rbx,qword ptr [r14]
  000000014094C0A2: movzx       ecx,byte ptr [rbx+48h]
  000000014094C0A6: mov         edx,dword ptr [r14+1Ch]
  000000014094C0AA: mov         r8,qword ptr [r14+10h]
  000000014094C0AE: mov         dword ptr [r8],edx
  000000014094C0B1: xor         cl,1
  000000014094C0B4: mov         byte ptr [rbx+48h],cl
  000000014094C0B7: test        cl,1
  000000014094C0BA: jne         000000014094AC51
  000000014094C0C0: test        r15b,r15b
  000000014094C0C3: je          000000014094C10C
  000000014094C0C5: call        _ZN6puffin14ThreadProfiler4call15THREAD_PROFILER29_$u7b$$u7b$constant$u7d$$u7d$28_$u7b$$u7b$closure$u7d$$u7d$31VAL$u7b$$u7b$tls.shim$u7d$$u7d$17h96833e27ced669c2E
  000000014094C0CA: mov         rcx,rax
  000000014094C0CD: mov         rax,qword ptr [rax]
  000000014094C0D0: cmp         rax,1
  000000014094C0D4: jne         000000014094C1A2
  000000014094C0DA: add         rcx,8
  000000014094C0DE: cmp         qword ptr [rcx],0
  000000014094C0E2: jne         000000014094C1D7
  000000014094C0E8: mov         qword ptr [rcx],0FFFFFFFFFFFFFFFFh
  000000014094C0EF: mov         qword ptr [rbp+1B8h],rcx
  000000014094C0F6: add         rcx,8
  000000014094C0FA: mov         rdx,rsi
  000000014094C0FD: call        _ZN6puffin14ThreadProfiler9end_scope17hbdd5f34d320e5739E
  000000014094C102: mov         rax,qword ptr [rbp+1B8h]
  000000014094C109: inc         qword ptr [rax]
  000000014094C10C: movaps      xmm6,xmmword ptr [rbp+1D0h]
  000000014094C113: movaps      xmm7,xmmword ptr [rbp+1E0h]
  000000014094C11A: movaps      xmm8,xmmword ptr [rbp+1F0h]
  000000014094C122: movaps      xmm9,xmmword ptr [rbp+200h]
  000000014094C12A: movaps      xmm10,xmmword ptr [rbp+210h]
  000000014094C132: movaps      xmm11,xmmword ptr [rbp+220h]
  000000014094C13A: movaps      xmm12,xmmword ptr [rbp+230h]
  000000014094C142: movaps      xmm13,xmmword ptr [rbp+240h]
  000000014094C14A: movaps      xmm14,xmmword ptr [rbp+250h]
  000000014094C152: movaps      xmm15,xmmword ptr [rbp+260h]
  000000014094C15A: add         rsp,2F8h
  000000014094C161: pop         rbx
  000000014094C162: pop         rdi
  000000014094C163: pop         rsi
  000000014094C164: pop         r12
  000000014094C166: pop         r13
  000000014094C168: pop         r14
  000000014094C16A: pop         r15
  000000014094C16C: pop         rbp
  000000014094C16D: ret
  000000014094C16E: test        rax,rax
  000000014094C171: jne         000000014094C1BA
  000000014094C173: xor         edx,edx
  000000014094C175: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h722048566c3776ebE
  000000014094C17A: mov         rcx,rax
  000000014094C17D: test        rax,rax
  000000014094C180: jne         000000014094ABA6
  000000014094C186: jmp         000000014094C1BA
  000000014094C188: test        rax,rax
  000000014094C18B: jne         000000014094C1BA
  000000014094C18D: xor         edx,edx
  000000014094C18F: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h722048566c3776ebE
  000000014094C194: mov         rcx,rax
  000000014094C197: test        rax,rax
  000000014094C19A: jne         000000014094C075
  000000014094C1A0: jmp         000000014094C1BA
  000000014094C1A2: test        rax,rax
  000000014094C1A5: jne         000000014094C1BA
  000000014094C1A7: xor         edx,edx
  000000014094C1A9: call        _ZN3std3sys12thread_local6native4lazy20Storage$LT$T$C$D$GT$10initialize17h722048566c3776ebE
  000000014094C1AE: mov         rcx,rax
  000000014094C1B1: test        rax,rax
  000000014094C1B4: jne         000000014094C0DE
  000000014094C1BA: lea         rcx,[anon.2d03173be639baf517cc66bf2fb7161a.11.llvm.10590071940455385864]
  000000014094C1C1: call        _ZN3std6thread5local18panic_access_error17hcff639665d708376E
  000000014094C1C6: lea         rcx,[142AB3700h]
  000000014094C1CD: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094C1D2: jmp         000000014094C29D
  000000014094C1D7: lea         rcx,[anon.2d03173be639baf517cc66bf2fb7161a.18.llvm.10590071940455385864]
  000000014094C1DE: call        _ZN4core4cell22panic_already_borrowed17h54fda569c7a6ddb1E
  000000014094C1E3: lea         rcx,[anon.8b73eb39c06afd88b250ecc24c523fbb.44.llvm.11914376241322277501]
  000000014094C1EA: lea         r8,[142AB36D0h]
  000000014094C1F1: mov         edx,32h
  000000014094C1F6: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094C1FB: jmp         000000014094C29D
  000000014094C200: lea         rcx,[anon.8b73eb39c06afd88b250ecc24c523fbb.44.llvm.11914376241322277501]
  000000014094C207: lea         r8,[142AB36E8h]
  000000014094C20E: mov         edx,32h
  000000014094C213: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094C218: jmp         000000014094C29D
  000000014094C21D: lea         rcx,[142AB3718h]
  000000014094C224: lea         r8,[142AB3750h]
  000000014094C22B: mov         edx,35h
  000000014094C230: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094C235: jmp         000000014094C29D
  000000014094C237: lea         r8,[142AB36B8h]
  000000014094C23E: mov         rdx,rsi
  000000014094C241: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094C246: jmp         000000014094C29D
  000000014094C248: lea         rcx,[anon.65ceffb69360efd323e48f4f8756afaa.25.llvm.13316938065337964131]
  000000014094C24F: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094C254: jmp         000000014094C29D
  000000014094C256: lea         rcx,[anon.65ceffb69360efd323e48f4f8756afaa.25.llvm.13316938065337964131]
  000000014094C25D: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094C262: jmp         000000014094C29D
  000000014094C264: lea         rcx,[anon.65ceffb69360efd323e48f4f8756afaa.25.llvm.13316938065337964131]
  000000014094C26B: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094C270: jmp         000000014094C29D
  000000014094C272: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.44.llvm.12803187488448158381]
  000000014094C279: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094C27E: jmp         000000014094C29D
  000000014094C280: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.42.llvm.12803187488448158381]
  000000014094C287: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094C28C: jmp         000000014094C29D
  000000014094C28E: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.43.llvm.12803187488448158381]
  000000014094C295: mov         rcx,rax
  000000014094C298: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094C29D: ud2
  000000014094C29F: nop
  000000014094C2A0: mov         qword ptr [rsp+10h],rdx
  000000014094C2A5: push        rbp
  000000014094C2A6: push        r15
  000000014094C2A8: push        r14
  000000014094C2AA: push        r13
  000000014094C2AC: push        r12
  000000014094C2AE: push        rsi
  000000014094C2AF: push        rdi
  000000014094C2B0: push        rbx
  000000014094C2B1: sub         rsp,0D8h
  000000014094C2B8: lea         rbp,[rdx+80h]
  000000014094C2BF: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C2C5: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C2CB: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C2D1: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C2D7: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C2DD: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C2E6: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C2EF: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C2F8: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C300: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C308: mov         rax,qword ptr [rbp+148h]
  000000014094C30F: inc         qword ptr [rax]
  000000014094C312: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C31A: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C322: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C32B: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C334: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C33D: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C343: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C349: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C34F: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C355: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C35B: add         rsp,0D8h
  000000014094C362: pop         rbx
  000000014094C363: pop         rdi
  000000014094C364: pop         rsi
  000000014094C365: pop         r12
  000000014094C367: pop         r13
  000000014094C369: pop         r14
  000000014094C36B: pop         r15
  000000014094C36D: pop         rbp
  000000014094C36E: ret
  000000014094C36F: nop
  000000014094C370: mov         qword ptr [rsp+10h],rdx
  000000014094C375: push        rbp
  000000014094C376: push        r15
  000000014094C378: push        r14
  000000014094C37A: push        r13
  000000014094C37C: push        r12
  000000014094C37E: push        rsi
  000000014094C37F: push        rdi
  000000014094C380: push        rbx
  000000014094C381: sub         rsp,0D8h
  000000014094C388: lea         rbp,[rdx+80h]
  000000014094C38F: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C395: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C39B: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C3A1: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C3A7: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C3AD: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C3B6: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C3BF: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C3C8: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C3D0: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C3D8: cmp         qword ptr [rbp+78h],0
  000000014094C3DD: je          000000014094C402
  000000014094C3DF: lea         rax,[rbp+0B0h]
  000000014094C3E6: mov         qword ptr [rbp-20h],rax
  000000014094C3EA: lea         rax,[rbp-20h]
  000000014094C3EE: mov         qword ptr [rbp-28h],rax
  000000014094C3F2: lea         rcx,[142AB33D8h]
  000000014094C3F9: lea         rdx,[rbp-28h]
  000000014094C3FD: call        _ZN3std6thread5local17LocalKey$LT$T$GT$4with17h8a9346824aaf9100E
  000000014094C402: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C40A: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C412: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C41B: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C424: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C42D: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C433: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C439: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C43F: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C445: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C44B: add         rsp,0D8h
  000000014094C452: pop         rbx
  000000014094C453: pop         rdi
  000000014094C454: pop         rsi
  000000014094C455: pop         r12
  000000014094C457: pop         r13
  000000014094C459: pop         r14
  000000014094C45B: pop         r15
  000000014094C45D: pop         rbp
  000000014094C45E: ret
  000000014094C45F: nop
  000000014094C460: mov         qword ptr [rsp+10h],rdx
  000000014094C465: push        rbp
  000000014094C466: push        r15
  000000014094C468: push        r14
  000000014094C46A: push        r13
  000000014094C46C: push        r12
  000000014094C46E: push        rsi
  000000014094C46F: push        rdi
  000000014094C470: push        rbx
  000000014094C471: sub         rsp,0D8h
  000000014094C478: lea         rbp,[rdx+80h]
  000000014094C47F: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C485: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C48B: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C491: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C497: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C49D: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C4A6: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C4AF: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C4B8: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C4C0: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C4C8: mov         rax,qword ptr [rbp+1B8h]
  000000014094C4CF: inc         qword ptr [rax]
  000000014094C4D2: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C4DA: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C4E2: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C4EB: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C4F4: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C4FD: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C503: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C509: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C50F: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C515: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C51B: add         rsp,0D8h
  000000014094C522: pop         rbx
  000000014094C523: pop         rdi
  000000014094C524: pop         rsi
  000000014094C525: pop         r12
  000000014094C527: pop         r13
  000000014094C529: pop         r14
  000000014094C52B: pop         r15
  000000014094C52D: pop         rbp
  000000014094C52E: ret
  000000014094C52F: nop
  000000014094C530: mov         qword ptr [rsp+10h],rdx
  000000014094C535: push        rbp
  000000014094C536: push        r15
  000000014094C538: push        r14
  000000014094C53A: push        r13
  000000014094C53C: push        r12
  000000014094C53E: push        rsi
  000000014094C53F: push        rdi
  000000014094C540: push        rbx
  000000014094C541: sub         rsp,0D8h
  000000014094C548: lea         rbp,[rdx+80h]
  000000014094C54F: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C555: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C55B: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C561: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C567: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C56D: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C576: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C57F: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C588: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C590: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C598: mov         rdx,qword ptr [rbp+0C8h]
  000000014094C59F: test        rdx,rdx
  000000014094C5A2: je          000000014094C5BA
  000000014094C5A4: mov         rcx,qword ptr [rbp+0D0h]
  000000014094C5AB: shl         rdx,3
  000000014094C5AF: mov         r8d,8
  000000014094C5B5: call        __rust_dealloc
  000000014094C5BA: mov         rdx,qword ptr [rbp+48h]
  000000014094C5BE: test        rdx,rdx
  000000014094C5C1: je          000000014094C5D6
  000000014094C5C3: mov         rcx,qword ptr [rbp+50h]
  000000014094C5C7: shl         rdx,3
  000000014094C5CB: mov         r8d,8
  000000014094C5D1: call        __rust_dealloc
  000000014094C5D6: mov         rax,qword ptr [rbp+0A8h]
  000000014094C5DD: mov         qword ptr [rbp+78h],rax
  000000014094C5E1: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C5E9: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C5F1: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C5FA: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C603: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C60C: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C612: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C618: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C61E: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C624: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C62A: add         rsp,0D8h
  000000014094C631: pop         rbx
  000000014094C632: pop         rdi
  000000014094C633: pop         rsi
  000000014094C634: pop         r12
  000000014094C636: pop         r13
  000000014094C638: pop         r14
  000000014094C63A: pop         r15
  000000014094C63C: pop         rbp
  000000014094C63D: ret
  000000014094C63E: nop
  000000014094C640: mov         qword ptr [rsp+10h],rdx
  000000014094C645: push        rbp
  000000014094C646: push        r15
  000000014094C648: push        r14
  000000014094C64A: push        r13
  000000014094C64C: push        r12
  000000014094C64E: push        rsi
  000000014094C64F: push        rdi
  000000014094C650: push        rbx
  000000014094C651: sub         rsp,0D8h
  000000014094C658: lea         rbp,[rdx+80h]
  000000014094C65F: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C665: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C66B: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C671: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C677: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C67D: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C686: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C68F: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C698: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C6A0: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C6A8: mov         rax,qword ptr [rbp+1B8h]
  000000014094C6AF: inc         qword ptr [rax]
  000000014094C6B2: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C6BA: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C6C2: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C6CB: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C6D4: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C6DD: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C6E3: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C6E9: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C6EF: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C6F5: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C6FB: add         rsp,0D8h
  000000014094C702: pop         rbx
  000000014094C703: pop         rdi
  000000014094C704: pop         rsi
  000000014094C705: pop         r12
  000000014094C707: pop         r13
  000000014094C709: pop         r14
  000000014094C70B: pop         r15
  000000014094C70D: pop         rbp
  000000014094C70E: ret
  000000014094C70F: nop
  000000014094C710: mov         qword ptr [rsp+10h],rdx
  000000014094C715: push        rbp
  000000014094C716: push        r15
  000000014094C718: push        r14
  000000014094C71A: push        r13
  000000014094C71C: push        r12
  000000014094C71E: push        rsi
  000000014094C71F: push        rdi
  000000014094C720: push        rbx
  000000014094C721: sub         rsp,0D8h
  000000014094C728: lea         rbp,[rdx+80h]
  000000014094C72F: movaps      xmmword ptr [rsp+30h],xmm15
  000000014094C735: movaps      xmmword ptr [rsp+40h],xmm14
  000000014094C73B: movaps      xmmword ptr [rsp+50h],xmm13
  000000014094C741: movaps      xmmword ptr [rsp+60h],xmm12
  000000014094C747: movaps      xmmword ptr [rsp+70h],xmm11
  000000014094C74D: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094C756: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094C75F: movaps      xmmword ptr [rsp+0A0h],xmm8
  000000014094C768: movaps      xmmword ptr [rsp+0B0h],xmm7
  000000014094C770: movaps      xmmword ptr [rsp+0C0h],xmm6
  000000014094C778: mov         rax,qword ptr [rbp+108h]
  000000014094C77F: test        rax,rax
  000000014094C782: je          000000014094C79E
  000000014094C784: shl         rax,3
  000000014094C788: lea         rdx,[rax+rax*2]
  000000014094C78C: mov         r8d,8
  000000014094C792: mov         rcx,qword ptr [rbp+0E0h]
  000000014094C799: call        __rust_dealloc
  000000014094C79E: movaps      xmm6,xmmword ptr [rsp+0C0h]
  000000014094C7A6: movaps      xmm7,xmmword ptr [rsp+0B0h]
  000000014094C7AE: movaps      xmm8,xmmword ptr [rsp+0A0h]
  000000014094C7B7: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094C7C0: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094C7C9: movaps      xmm11,xmmword ptr [rsp+70h]
  000000014094C7CF: movaps      xmm12,xmmword ptr [rsp+60h]
  000000014094C7D5: movaps      xmm13,xmmword ptr [rsp+50h]
  000000014094C7DB: movaps      xmm14,xmmword ptr [rsp+40h]
  000000014094C7E1: movaps      xmm15,xmmword ptr [rsp+30h]
  000000014094C7E7: add         rsp,0D8h
  000000014094C7EE: pop         rbx
  000000014094C7EF: pop         rdi
  000000014094C7F0: pop         rsi
  000000014094C7F1: pop         r12
  000000014094C7F3: pop         r13
  000000014094C7F5: pop         r14
  000000014094C7F7: pop         r15
  000000014094C7F9: pop         rbp
  000000014094C7FA: ret
  000000014094C7FB: CC CC CC CC CC                                   .....

; ===== _ZN12country_core7systems3ivy10ivy_grower21closest_segment_index17hba4d5971b8993167E:
  000000014094C800: push        r14
  000000014094C802: push        rsi
  000000014094C803: push        rdi
  000000014094C804: push        rbx
  000000014094C805: sub         rsp,0D8h
  000000014094C80C: movaps      xmmword ptr [rsp+0C0h],xmm15
  000000014094C815: movaps      xmmword ptr [rsp+0B0h],xmm14
  000000014094C81E: movaps      xmmword ptr [rsp+0A0h],xmm13
  000000014094C827: movaps      xmmword ptr [rsp+90h],xmm12
  000000014094C830: movaps      xmmword ptr [rsp+80h],xmm11
  000000014094C839: movaps      xmmword ptr [rsp+70h],xmm10
  000000014094C83F: movaps      xmmword ptr [rsp+60h],xmm9
  000000014094C845: movaps      xmmword ptr [rsp+50h],xmm8
  000000014094C84B: movaps      xmmword ptr [rsp+40h],xmm7
  000000014094C850: movaps      xmmword ptr [rsp+30h],xmm6
  000000014094C855: mov         rax,qword ptr [rsp+120h]
  000000014094C85D: test        rax,rax
  000000014094C860: je          000000014094CA0F
  000000014094C866: movss       xmm6,dword ptr [rsp+128h]
  000000014094C86F: imul        rsi,rax,38h
  000000014094C873: add         rsi,r9
  000000014094C876: unpcklps    xmm1,xmm2
  000000014094C879: movss       xmm7,dword ptr [__real@42c80000]
  000000014094C881: xor         eax,eax
  000000014094C883: movss       xmm8,dword ptr [__real@1e3ce508]
  000000014094C88C: movss       xmm10,dword ptr [__real@3f800000]
  000000014094C895: movaps      xmm11,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  000000014094C89D: movaps      xmm4,xmm7
  000000014094C8A0: xor         edi,edi
  000000014094C8A2: nop         word ptr cs:[rax+rax]
  000000014094C8B0: movss       xmm14,dword ptr [r9+18h]
  000000014094C8B6: movss       xmm15,dword ptr [r9+1Ch]
  000000014094C8BC: ucomiss     xmm14,xmm6
  000000014094C8C0: jbe         000000014094C8CC
  000000014094C8C2: ucomiss     xmm15,xmm6
  000000014094C8C6: ja          000000014094C95E
  000000014094C8CC: movsd       xmm5,mmword ptr [r9]
  000000014094C8D1: movsd       xmm13,mmword ptr [r9+8]
  000000014094C8D7: movaps      xmm2,xmm1
  000000014094C8DA: subps       xmm2,xmm5
  000000014094C8DD: subps       xmm13,xmm5
  000000014094C8E1: mulps       xmm2,xmm13
  000000014094C8E5: movshdup    xmm12,xmm2
  000000014094C8EA: addss       xmm12,xmm2
  000000014094C8EF: movaps      xmm2,xmm13
  000000014094C8F3: mulps       xmm2,xmm13
  000000014094C8F7: movshdup    xmm9,xmm2
  000000014094C8FC: addss       xmm9,xmm2
  000000014094C901: movaps      xmm2,xmm9
  000000014094C905: maxss       xmm2,xmm8
  000000014094C90A: divss       xmm12,xmm2
  000000014094C90F: xorps       xmm3,xmm3
  000000014094C912: maxss       xmm3,xmm12
  000000014094C917: movaps      xmm2,xmm10
  000000014094C91B: minss       xmm2,xmm3
  000000014094C91F: movaps      xmm3,xmm10
  000000014094C923: subss       xmm3,xmm2
  000000014094C927: mulss       xmm14,xmm3
  000000014094C92C: mulss       xmm15,xmm2
  000000014094C931: addss       xmm15,xmm14
  000000014094C936: ucomiss     xmm15,xmm6
  000000014094C93A: ja          000000014094C95E
  000000014094C93C: sqrtss      xmm9,xmm9
  000000014094C941: movaps      xmm14,xmm2
  000000014094C945: subss       xmm14,xmm2
  000000014094C94A: andps       xmm14,xmm11
  000000014094C94E: mulss       xmm14,xmm9
  000000014094C953: mulss       xmm14,xmm7
  000000014094C958: ucomiss     xmm14,xmm4
  000000014094C95C: jb          000000014094C973
  000000014094C95E: add         r9,38h
  000000014094C962: inc         rdi
  000000014094C965: cmp         r9,rsi
  000000014094C968: jne         000000014094C8B0
  000000014094C96E: jmp         000000014094CA11
  000000014094C973: movshdup    xmm3,xmm13
  000000014094C978: xorps       xmm3,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094C97F: subps       xmm5,xmm1
  000000014094C982: unpcklps    xmm3,xmm13
  000000014094C986: movsldup    xmm9,xmm9
  000000014094C98B: divps       xmm3,xmm9
  000000014094C98F: mulps       xmm3,xmm5
  000000014094C992: movshdup    xmm13,xmm3
  000000014094C997: addss       xmm13,xmm3
  000000014094C99C: andps       xmm13,xmm11
  000000014094C9A0: addss       xmm13,xmm14
  000000014094C9A5: ucomiss     xmm4,xmm13
  000000014094C9A9: jbe         000000014094C9FD
  000000014094C9AB: ucomiss     xmm2,dword ptr [__real@00000000]
  000000014094C9B2: jb          000000014094CA81
  000000014094C9B8: ucomiss     xmm10,xmm2
  000000014094C9BC: jb          000000014094CA81
  000000014094C9C2: movaps      xmm14,xmm1
  000000014094C9C6: mov         rbx,rcx
  000000014094C9C9: movss       xmm0,dword ptr [r9+10h]
  000000014094C9CF: movss       xmm1,dword ptr [r9+14h]
  000000014094C9D5: mov         r14,r9
  000000014094C9D8: call        _ZN5utils4lerp17hf3285295543ed03fE
  000000014094C9DD: mov         r9,r14
  000000014094C9E0: movss       dword ptr [rsp+2Ch],xmm0
  000000014094C9E6: mov         eax,1
  000000014094C9EB: mov         rdx,rdi
  000000014094C9EE: movaps      xmm0,xmm13
  000000014094C9F2: movaps      xmm4,xmm13
  000000014094C9F6: mov         rcx,rbx
  000000014094C9F9: movaps      xmm1,xmm14
  000000014094C9FD: inc         rdi
  000000014094CA00: add         r9,38h
  000000014094CA04: cmp         r9,rsi
  000000014094CA07: jne         000000014094C8B0
  000000014094CA0D: jmp         000000014094CA11
  000000014094CA0F: xor         eax,eax
  000000014094CA11: mov         qword ptr [rcx],rax
  000000014094CA14: mov         qword ptr [rcx+8],rdx
  000000014094CA18: movss       dword ptr [rcx+10h],xmm0
  000000014094CA1D: movss       xmm0,dword ptr [rsp+2Ch]
  000000014094CA23: movss       dword ptr [rcx+14h],xmm0
  000000014094CA28: mov         rax,rcx
  000000014094CA2B: movaps      xmm6,xmmword ptr [rsp+30h]
  000000014094CA30: movaps      xmm7,xmmword ptr [rsp+40h]
  000000014094CA35: movaps      xmm8,xmmword ptr [rsp+50h]
  000000014094CA3B: movaps      xmm9,xmmword ptr [rsp+60h]
  000000014094CA41: movaps      xmm10,xmmword ptr [rsp+70h]
  000000014094CA47: movaps      xmm11,xmmword ptr [rsp+80h]
  000000014094CA50: movaps      xmm12,xmmword ptr [rsp+90h]
  000000014094CA59: movaps      xmm13,xmmword ptr [rsp+0A0h]
  000000014094CA62: movaps      xmm14,xmmword ptr [rsp+0B0h]
  000000014094CA6B: movaps      xmm15,xmmword ptr [rsp+0C0h]
  000000014094CA74: add         rsp,0D8h
  000000014094CA7B: pop         rbx
  000000014094CA7C: pop         rdi
  000000014094CA7D: pop         rsi
  000000014094CA7E: pop         r14
  000000014094CA80: ret
  000000014094CA81: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.15.llvm.3277990281340110158]
  000000014094CA88: lea         r8,[anon.12ce4fde5425384d5f438f5122902e1a.17.llvm.3277990281340110158]
  000000014094CA8F: mov         edx,2Ah
  000000014094CA94: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094CA99: int         3
  000000014094CA9A: CC CC CC CC CC CC                                ......

; ===== _ZN12country_core7systems3ivy10ivy_grower28get_wall_double_sided_normal17h58918def05de80b9E:
  000000014094CAA0: push        r15
  000000014094CAA2: push        r14
  000000014094CAA4: push        rsi
  000000014094CAA5: push        rdi
  000000014094CAA6: push        rbx
  000000014094CAA7: sub         rsp,90h
  000000014094CAAE: movaps      xmmword ptr [rsp+80h],xmm10
  000000014094CAB7: movaps      xmmword ptr [rsp+70h],xmm9
  000000014094CABD: movaps      xmmword ptr [rsp+60h],xmm8
  000000014094CAC3: movaps      xmmword ptr [rsp+50h],xmm7
  000000014094CAC8: movaps      xmmword ptr [rsp+40h],xmm6
  000000014094CACD: mov         r14,r9
  000000014094CAD0: movaps      xmm6,xmm2
  000000014094CAD3: mov         rbx,rdx
  000000014094CAD6: mov         rsi,rcx
  000000014094CAD9: mov         rcx,rdx
  000000014094CADC: movaps      xmm1,xmm2
  000000014094CADF: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094CAE4: mov         eax,eax
  000000014094CAE6: lea         rcx,[rax+1]
  000000014094CAEA: mov         rdi,qword ptr [rbx+10h]
  000000014094CAEE: cmp         rcx,rdi
  000000014094CAF1: jae         000000014094CC2F
  000000014094CAF7: mov         r15,qword ptr [rbx+8]
  000000014094CAFB: movsd       xmm0,mmword ptr [r15+rax*8]
  000000014094CB01: movsd       xmm1,mmword ptr [r15+rax*8+8]
  000000014094CB08: subps       xmm1,xmm0
  000000014094CB0B: movaps      xmm0,xmm1
  000000014094CB0E: mulps       xmm0,xmm1
  000000014094CB11: movshdup    xmm2,xmm0
  000000014094CB15: addss       xmm2,xmm0
  000000014094CB19: xorps       xmm0,xmm0
  000000014094CB1C: sqrtss      xmm0,xmm2
  000000014094CB20: movss       xmm9,dword ptr [__real@3f800000]
  000000014094CB29: movaps      xmm2,xmm9
  000000014094CB2D: divss       xmm2,xmm0
  000000014094CB31: movsldup    xmm7,xmm2
  000000014094CB35: mulps       xmm7,xmm1
  000000014094CB38: movshdup    xmm0,xmm7
  000000014094CB3C: xorps       xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094CB43: movss       dword ptr [rsp+28h],xmm0
  000000014094CB49: movss       dword ptr [rsp+2Ch],xmm7
  000000014094CB4F: movss       xmm8,dword ptr [r14]
  000000014094CB54: movss       xmm10,dword ptr [r14+8]
  000000014094CB5A: mov         rcx,rbx
  000000014094CB5D: movaps      xmm1,xmm6
  000000014094CB60: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094CB65: mov         ecx,eax
  000000014094CB67: cmp         rdi,rcx
  000000014094CB6A: jbe         000000014094CC3E
  000000014094CB70: lea         rax,[rcx+1]
  000000014094CB74: cmp         rax,rdi
  000000014094CB77: jae         000000014094CC4D
  000000014094CB7D: subss       xmm9,xmm0
  000000014094CB82: movsd       xmm1,mmword ptr [r15+rcx*8]
  000000014094CB88: movsd       xmm2,mmword ptr [r15+rcx*8+8]
  000000014094CB8F: movsldup    xmm3,xmm9
  000000014094CB94: mulps       xmm3,xmm1
  000000014094CB97: movsldup    xmm0,xmm0
  000000014094CB9B: mulps       xmm0,xmm2
  000000014094CB9E: addps       xmm0,xmm3
  000000014094CBA1: unpcklps    xmm8,xmm10
  000000014094CBA5: shufps      xmm0,xmm0,0E1h
  000000014094CBA9: shufps      xmm8,xmm8,0E1h
  000000014094CBAE: subps       xmm8,xmm0
  000000014094CBB2: mulps       xmm7,xmm8
  000000014094CBB6: movshdup    xmm0,xmm7
  000000014094CBBA: ucomiss     xmm0,xmm7
  000000014094CBBD: jbe         000000014094CBF1
  000000014094CBBF: lea         rcx,[rsp+30h]
  000000014094CBC4: lea         rdx,[rsp+28h]
  000000014094CBC9: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094CBCE: movss       xmm0,dword ptr [rsp+38h]
  000000014094CBD4: movaps      xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094CBDB: xorps       xmm0,xmm1
  000000014094CBDE: movsd       xmm2,mmword ptr [rsp+30h]
  000000014094CBE4: xorps       xmm2,xmm1
  000000014094CBE7: movlps      qword ptr [rsi],xmm2
  000000014094CBEA: movss       dword ptr [rsi+8],xmm0
  000000014094CBEF: jmp         000000014094CBFE
  000000014094CBF1: lea         rdx,[rsp+28h]
  000000014094CBF6: mov         rcx,rsi
  000000014094CBF9: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094CBFE: mov         rax,rsi
  000000014094CC01: movaps      xmm6,xmmword ptr [rsp+40h]
  000000014094CC06: movaps      xmm7,xmmword ptr [rsp+50h]
  000000014094CC0B: movaps      xmm8,xmmword ptr [rsp+60h]
  000000014094CC11: movaps      xmm9,xmmword ptr [rsp+70h]
  000000014094CC17: movaps      xmm10,xmmword ptr [rsp+80h]
  000000014094CC20: add         rsp,90h
  000000014094CC27: pop         rbx
  000000014094CC28: pop         rdi
  000000014094CC29: pop         rsi
  000000014094CC2A: pop         r14
  000000014094CC2C: pop         r15
  000000014094CC2E: ret
  000000014094CC2F: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.44.llvm.12803187488448158381]
  000000014094CC36: mov         rdx,rdi
  000000014094CC39: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094CC3E: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.42.llvm.12803187488448158381]
  000000014094CC45: mov         rdx,rdi
  000000014094CC48: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094CC4D: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.43.llvm.12803187488448158381]
  000000014094CC54: mov         rcx,rax
  000000014094CC57: mov         rdx,rdi
  000000014094CC5A: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094CC5F: int         3

; ===== _ZN12country_core7systems3ivy10ivy_grower35intersect_ivy_growth_w_wall_segment17h7ffb7df40688276dE:
  000000014094CC60: push        rsi
  000000014094CC61: sub         rsp,0F0h
  000000014094CC68: movaps      xmmword ptr [rsp+0E0h],xmm14
  000000014094CC71: movaps      xmmword ptr [rsp+0D0h],xmm13
  000000014094CC7A: movaps      xmmword ptr [rsp+0C0h],xmm12
  000000014094CC83: movaps      xmmword ptr [rsp+0B0h],xmm11
  000000014094CC8C: movaps      xmmword ptr [rsp+0A0h],xmm10
  000000014094CC95: movaps      xmmword ptr [rsp+90h],xmm9
  000000014094CC9E: movaps      xmmword ptr [rsp+80h],xmm8
  000000014094CCA7: movaps      xmmword ptr [rsp+70h],xmm7
  000000014094CCAC: movaps      xmmword ptr [rsp+60h],xmm6
  000000014094CCB1: movaps      xmm6,xmm2
  000000014094CCB4: movsd       xmm8,mmword ptr [rdx]
  000000014094CCB9: movsd       xmm7,mmword ptr [rdx+8]
  000000014094CCBE: movaps      xmm9,xmm8
  000000014094CCC2: addps       xmm9,xmm7
  000000014094CCC6: mulps       xmm9,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094CCCE: mov         rsi,rcx
  000000014094CCD1: movaps      xmm11,xmm7
  000000014094CCD5: subps       xmm11,xmm8
  000000014094CCD9: movaps      xmm0,xmm11
  000000014094CCDD: mulps       xmm0,xmm11
  000000014094CCE1: movshdup    xmm10,xmm0
  000000014094CCE6: addss       xmm10,xmm0
  000000014094CCEB: xorps       xmm12,xmm12
  000000014094CCEF: sqrtss      xmm12,xmm10
  000000014094CCF4: movsldup    xmm0,xmm12
  000000014094CCF9: movaps      xmm1,xmm11
  000000014094CCFD: divps       xmm1,xmm0
  000000014094CD00: movshdup    xmm13,xmm1
  000000014094CD05: xorps       xmm13,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094CD0D: movlhps     xmm13,xmm1
  000000014094CD11: shufps      xmm13,xmm1,48h
  000000014094CD16: unpcklps    xmm6,xmm3
  000000014094CD19: movaps      xmm1,xmm6
  000000014094CD1C: subps       xmm1,xmm9
  000000014094CD20: unpcklps    xmm1,xmm1
  000000014094CD23: mulps       xmm1,xmm13
  000000014094CD27: movaps      xmm0,xmm1
  000000014094CD2A: unpckhpd    xmm0,xmm1
  000000014094CD2E: addps       xmm0,xmm1
  000000014094CD31: movshdup    xmm1,xmm0
  000000014094CD35: movss       xmm2,dword ptr [rsp+128h]
  000000014094CD3E: movss       xmm3,dword ptr [rsp+120h]
  000000014094CD47: shufps      xmm3,xmm2,0
  000000014094CD4B: movaps      xmm2,xmm9
  000000014094CD4F: unpcklps    xmm2,xmm9
  000000014094CD53: subps       xmm3,xmm2
  000000014094CD56: mulps       xmm3,xmm13
  000000014094CD5A: movaps      xmm4,xmm3
  000000014094CD5D: unpckhpd    xmm4,xmm3
  000000014094CD61: addps       xmm4,xmm3
  000000014094CD64: movshdup    xmm3,xmm4
  000000014094CD68: movss       xmm2,dword ptr [__real@bf000000]
  000000014094CD70: mulss       xmm2,xmm12
  000000014094CD75: movss       xmm5,dword ptr [__real@3f000000]
  000000014094CD7D: mulss       xmm5,xmm12
  000000014094CD82: movss       dword ptr [rsp+20h],xmm5
  000000014094CD88: movss       dword ptr [rsp+38h],xmm4
  000000014094CD8E: movss       dword ptr [rsp+28h],xmm0
  000000014094CD94: movss       dword ptr [rsp+40h],xmm3
  000000014094CD9A: movss       dword ptr [rsp+30h],xmm1
  000000014094CDA0: lea         rcx,[rsp+4Ch]
  000000014094CDA5: movss       xmm1,dword ptr [__real@bea147ae]
  000000014094CDAD: movss       xmm3,dword ptr [__real@3ea147ae]
  000000014094CDB5: call        _ZN5utils8geometry4aabb26intersection_aabb_segment217h905d1a772d445ae0E
  000000014094CDBA: cmp         dword ptr [rsp+4Ch],1
  000000014094CDBF: jne         000000014094CF2D
  000000014094CDC5: movsd       xmm0,mmword ptr [rsp+50h]
  000000014094CDCB: unpcklps    xmm0,xmm0
  000000014094CDCE: mulps       xmm13,xmm0
  000000014094CDD2: movaps      xmm14,xmm13
  000000014094CDD6: unpckhpd    xmm14,xmm13
  000000014094CDDB: addps       xmm14,xmm13
  000000014094CDDF: movss       xmm13,dword ptr [__real@3f800000]
  000000014094CDE8: movaps      xmm0,xmm13
  000000014094CDEC: divss       xmm0,xmm12
  000000014094CDF1: movsldup    xmm0,xmm0
  000000014094CDF5: mulps       xmm0,xmm11
  000000014094CDF9: movlps      qword ptr [rsp+58h],xmm0
  000000014094CDFE: lea         rcx,[rsp+4Ch]
  000000014094CE03: lea         rdx,[rsp+58h]
  000000014094CE08: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094CE0D: movsd       xmm0,mmword ptr [rsp+50h]
  000000014094CE13: movss       xmm1,dword ptr [rsp+4Ch]
  000000014094CE19: movlhps     xmm1,xmm0
  000000014094CE1C: shufps      xmm1,xmm0,0D8h
  000000014094CE20: movsd       xmm2,mmword ptr [__xmm@00000000000000003f3504f300000000]
  000000014094CE28: mulps       xmm2,xmm1
  000000014094CE2B: movshdup    xmm3,xmm2
  000000014094CE2F: addss       xmm3,xmm2
  000000014094CE33: movhlps     xmm2,xmm2
  000000014094CE36: addss       xmm2,xmm3
  000000014094CE3A: addss       xmm2,xmm2
  000000014094CE3E: xorps       xmm4,xmm4
  000000014094CE41: mulss       xmm2,xmm4
  000000014094CE45: shufps      xmm2,xmm2,0
  000000014094CE49: movss       xmm3,dword ptr [__real@3f3504f3]
  000000014094CE51: mulps       xmm3,xmm1
  000000014094CE54: addps       xmm9,xmm14
  000000014094CE58: subps       xmm9,xmm8
  000000014094CE5C: mulps       xmm9,xmm11
  000000014094CE60: movshdup    xmm5,xmm9
  000000014094CE65: addss       xmm5,xmm9
  000000014094CE6A: maxss       xmm10,dword ptr [__real@1e3ce508]
  000000014094CE73: divss       xmm5,xmm10
  000000014094CE78: maxss       xmm4,xmm5
  000000014094CE7C: movaps      xmm5,xmm13
  000000014094CE80: minss       xmm5,xmm4
  000000014094CE84: subss       xmm13,xmm5
  000000014094CE89: movsldup    xmm4,xmm13
  000000014094CE8E: mulps       xmm4,xmm8
  000000014094CE92: movsldup    xmm5,xmm5
  000000014094CE96: mulps       xmm7,xmm5
  000000014094CE99: addps       xmm7,xmm4
  000000014094CE9C: shufps      xmm1,xmm1,0E8h
  000000014094CEA0: xorps       xmm4,xmm4
  000000014094CEA3: mulps       xmm4,xmm1
  000000014094CEA6: addps       xmm4,xmm2
  000000014094CEA9: shufps      xmm3,xmm3,0D1h
  000000014094CEAD: shufps      xmm0,xmm0,0D1h
  000000014094CEB1: mulps       xmm0,xmmword ptr [__xmm@000000003f3504f3000000003f3504f3]
  000000014094CEB8: subps       xmm0,xmm3
  000000014094CEBB: mulps       xmm0,xmmword ptr [__xmm@000000003fb504f33fb504f33fb504f3]
  000000014094CEC2: addps       xmm0,xmm4
  000000014094CEC5: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f2147ae3f2147ae]
  000000014094CECC: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094CED3: movaps      xmm1,xmm7
  000000014094CED6: subps       xmm1,xmm0
  000000014094CED9: addps       xmm0,xmm7
  000000014094CEDC: movaps      xmm2,xmm1
  000000014094CEDF: subps       xmm2,xmm6
  000000014094CEE2: mulps       xmm2,xmm2
  000000014094CEE5: movshdup    xmm3,xmm2
  000000014094CEE9: addss       xmm3,xmm2
  000000014094CEED: movaps      xmm2,xmm0
  000000014094CEF0: subps       xmm2,xmm6
  000000014094CEF3: mulps       xmm2,xmm2
  000000014094CEF6: movshdup    xmm4,xmm2
  000000014094CEFA: addss       xmm4,xmm2
  000000014094CEFE: xor         eax,eax
  000000014094CF00: ucomiss     xmm4,xmm3
  000000014094CF03: seta        al
  000000014094CF06: movd        xmm2,eax
  000000014094CF0A: pshufd      xmm2,xmm2,50h
  000000014094CF0F: pslld       xmm2,1Fh
  000000014094CF14: psrad       xmm2,1Fh
  000000014094CF19: andps       xmm1,xmm2
  000000014094CF1C: andnps      xmm2,xmm0
  000000014094CF1F: orps        xmm2,xmm1
  000000014094CF22: movlps      qword ptr [rsi+4],xmm2
  000000014094CF26: mov         eax,1
  000000014094CF2B: jmp         000000014094CF2F
  000000014094CF2D: xor         eax,eax
  000000014094CF2F: mov         dword ptr [rsi],eax
  000000014094CF31: mov         rax,rsi
  000000014094CF34: movaps      xmm6,xmmword ptr [rsp+60h]
  000000014094CF39: movaps      xmm7,xmmword ptr [rsp+70h]
  000000014094CF3E: movaps      xmm8,xmmword ptr [rsp+80h]
  000000014094CF47: movaps      xmm9,xmmword ptr [rsp+90h]
  000000014094CF50: movaps      xmm10,xmmword ptr [rsp+0A0h]
  000000014094CF59: movaps      xmm11,xmmword ptr [rsp+0B0h]
  000000014094CF62: movaps      xmm12,xmmword ptr [rsp+0C0h]
  000000014094CF6B: movaps      xmm13,xmmword ptr [rsp+0D0h]
  000000014094CF74: movaps      xmm14,xmmword ptr [rsp+0E0h]
  000000014094CF7D: add         rsp,0F0h
  000000014094CF84: pop         rsi
  000000014094CF85: ret
  000000014094CF86: CC CC CC CC CC CC CC CC CC CC                    ..........

; ===== country_core::systems::ivy::ivy_pruner::is_point_still_valid:
  0000000140998DC0: push        r15
  0000000140998DC2: push        r14
  0000000140998DC4: push        r12
  0000000140998DC6: push        rsi
  0000000140998DC7: push        rdi
  0000000140998DC8: push        rbx
  0000000140998DC9: sub         rsp,128h
  0000000140998DD0: movaps      xmmword ptr [rsp+110h],xmm10
  0000000140998DD9: movaps      xmmword ptr [rsp+100h],xmm9
  0000000140998DE2: movaps      xmmword ptr [rsp+0F0h],xmm8
  0000000140998DEB: movaps      xmmword ptr [rsp+0E0h],xmm7
  0000000140998DF3: movaps      xmmword ptr [rsp+0D0h],xmm6
  0000000140998DFB: mov         rdi,r9
  0000000140998DFE: mov         r14,r8
  0000000140998E01: mov         rbx,rdx
  0000000140998E04: mov         rsi,rcx
  0000000140998E07: movss       xmm6,dword ptr [rcx]
  0000000140998E0B: movss       xmm7,dword ptr [rcx+8]
  0000000140998E10: movsd       xmm0,mmword ptr [rdx+50h]
  0000000140998E15: movaps      xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  0000000140998E1C: mulps       xmm1,xmm0
  0000000140998E1F: movaps      xmm8,xmm6
  0000000140998E23: unpcklps    xmm8,xmm7
  0000000140998E27: addps       xmm8,xmm1
  0000000140998E2B: movsd       xmm10,mmword ptr [rdx+48h]
  0000000140998E31: cvtdq2ps    xmm1,xmm10
  0000000140998E35: divps       xmm0,xmm1
  0000000140998E38: divps       xmm8,xmm0
  0000000140998E3C: movshdup    xmm0,xmm8
  0000000140998E41: call        floorf
  0000000140998E46: movaps      xmm9,xmm0
  0000000140998E4A: movaps      xmm0,xmm8
  0000000140998E4E: call        floorf
  0000000140998E53: cvttss2si   ecx,xmm0
  0000000140998E57: movss       xmm1,dword ptr [__real@4effffff]
  0000000140998E5F: ucomiss     xmm0,xmm1
  0000000140998E62: mov         r8d,7FFFFFFFh
  0000000140998E68: cmova       ecx,r8d
  0000000140998E6C: xor         eax,eax
  0000000140998E6E: ucomiss     xmm0,xmm0
  0000000140998E71: cmovp       ecx,eax
  0000000140998E74: cvttss2si   edx,xmm9
  0000000140998E79: ucomiss     xmm9,xmm1
  0000000140998E7D: cmova       edx,r8d
  0000000140998E81: ucomiss     xmm9,xmm9
  0000000140998E85: cmovp       edx,eax
  0000000140998E88: test        ecx,ecx
  0000000140998E8A: js          00000001409992BF
  0000000140998E90: movd        xmm0,ecx
  0000000140998E94: movd        xmm1,edx
  0000000140998E98: punpckldq   xmm0,xmm1
  0000000140998E9C: movq        xmm0,xmm0
  0000000140998EA0: movaps      xmm1,xmm10
  0000000140998EA4: pcmpgtd     xmm1,xmm0
  0000000140998EA8: pshufd      xmm1,xmm1,50h
  0000000140998EAD: movmskpd    edx,xmm1
  0000000140998EB1: xor         eax,eax
  0000000140998EB3: test        dl,2
  0000000140998EB6: je          00000001409992BF
  0000000140998EBC: test        dl,1
  0000000140998EBF: je          00000001409992BF
  0000000140998EC5: pshufd      xmm0,xmm0,55h
  0000000140998ECA: movd        edx,xmm0
  0000000140998ECE: test        edx,edx
  0000000140998ED0: js          00000001409992BF
  0000000140998ED6: movd        eax,xmm10
  0000000140998EDB: imul        edx,eax
  0000000140998EDE: add         edx,ecx
  0000000140998EE0: movsxd      rcx,edx
  0000000140998EE3: cmp         qword ptr [rbx+10h],rcx
  0000000140998EE7: jbe         00000001409992BD
  0000000140998EED: mov         rax,qword ptr [rbx+8]
  0000000140998EF1: lea         rcx,[rcx+rcx*2]
  0000000140998EF5: shl         rcx,4
  0000000140998EF9: mov         r15,qword ptr [rax+rcx+10h]
  0000000140998EFE: test        r15,r15
  0000000140998F01: je          00000001409992BD
  0000000140998F07: mov         rbx,qword ptr [rax+rcx+8]
  0000000140998F0C: mov         qword ptr [rsp+20h],r15
  0000000140998F11: mov         dword ptr [rsp+28h],3F000000h
  0000000140998F19: lea         rcx,[rsp+40h]
  0000000140998F1E: movaps      xmm1,xmm6
  0000000140998F21: movaps      xmm2,xmm7
  0000000140998F24: mov         r9,rbx
  0000000140998F27: call        _ZN12country_core7systems3ivy10ivy_grower21closest_segment_index17hba4d5971b8993167E
  0000000140998F2C: cmp         dword ptr [rsp+40h],1
  0000000140998F31: jne         00000001409992BD
  0000000140998F37: mov         rcx,qword ptr [rsp+48h]
  0000000140998F3C: movss       xmm0,dword ptr [rsp+50h]
  0000000140998F42: movss       xmm8,dword ptr [rsp+54h]
  0000000140998F49: mov         qword ptr [rsp+38h],rcx
  0000000140998F4E: cmp         rcx,r15
  0000000140998F51: jae         0000000140999313
  0000000140998F57: movss       xmm1,dword ptr [__real@3ebae148]
  0000000140998F5F: ucomiss     xmm1,xmm0
  0000000140998F62: jbe         00000001409992BD
  0000000140998F68: imul        rax,rcx,38h
  0000000140998F6C: mov         r15,qword ptr [rbx+rax+28h]
  0000000140998F71: mov         qword ptr [rsp+40h],r15
  0000000140998F76: cmp         qword ptr [r14+18h],0
  0000000140998F7B: je          0000000140999025
  0000000140998F81: add         rbx,rax
  0000000140998F84: lea         rcx,[r14+20h]
  0000000140998F88: lea         rdx,[rsp+40h]
  0000000140998F8D: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  0000000140998F92: mov         rcx,qword ptr [r14]
  0000000140998F95: mov         rdx,qword ptr [r14+8]
  0000000140998F99: mov         r8,rdx
  0000000140998F9C: and         r8,rax
  0000000140998F9F: shr         rax,39h
  0000000140998FA3: movd        xmm0,eax
  0000000140998FA7: punpcklbw   xmm0,xmm0
  0000000140998FAB: pshuflw     xmm0,xmm0,0
  0000000140998FB0: pshufd      xmm0,xmm0,0
  0000000140998FB5: lea         rax,[rcx-2A8h]
  0000000140998FBC: xor         r9d,r9d
  0000000140998FBF: pcmpeqd     xmm1,xmm1
  0000000140998FC3: movdqu      xmm2,xmmword ptr [rcx+r8]
  0000000140998FC9: movdqa      xmm3,xmm2
  0000000140998FCD: pcmpeqb     xmm3,xmm0
  0000000140998FD1: pmovmskb    r11d,xmm3
  0000000140998FD6: test        r11d,r11d
  0000000140998FD9: je          0000000140999007
  0000000140998FDB: tzcnt       r10d,r11d
  0000000140998FE0: add         r10,r8
  0000000140998FE3: and         r10,rdx
  0000000140998FE6: neg         r10
  0000000140998FE9: imul        r10,r10,2A8h
  0000000140998FF0: cmp         qword ptr [rax+r10],r15
  0000000140998FF4: je          0000000140999118
  0000000140998FFA: lea         r10d,[r11-1]
  0000000140998FFE: and         r10w,r11w
  0000000140999002: mov         r11d,r10d
  0000000140999005: jne         0000000140998FDB
  0000000140999007: pcmpeqb     xmm2,xmm1
  000000014099900B: pmovmskb    r10d,xmm2
  0000000140999010: test        r10d,r10d
  0000000140999013: jne         0000000140999025
  0000000140999015: add         r8,r9
  0000000140999018: add         r8,10h
  000000014099901C: add         r9,10h
  0000000140999020: and         r8,rdx
  0000000140999023: jmp         0000000140998FC3
  0000000140999025: mov         rax,qword ptr [__imp__ZN3log20MAX_LOG_LEVEL_FILTER17h0a1e497ddb29da86E]
  000000014099902C: mov         rax,qword ptr [rax]
  000000014099902F: cmp         rax,2
  0000000140999033: jb          00000001409992BD
  0000000140999039: lea         rax,[rsp+38h]
  000000014099903E: mov         qword ptr [rsp+0C0h],rax
  0000000140999046: lea         rax,[_ZN4core3fmt3num3imp52_$LT$impl$u20$core..fmt..Display$u20$for$u20$u64$GT$3fmt17hdda06e2292a0ed37E]
  000000014099904D: mov         qword ptr [rsp+0C8h],rax
  0000000140999055: lea         rcx,[142ABB048h]
  000000014099905C: call        _ZN100_$LT$country_core..resources..world_raster_defs..GardenRaster$u20$as$u20$core..ops..deref..Deref$GT$5deref17h37f159664114cf7aE
  0000000140999061: movdqu      xmm0,xmmword ptr [rax]
  0000000140999065: mov         eax,dword ptr [rax+10h]
  0000000140999068: mov         qword ptr [rsp+70h],2
  0000000140999071: lea         rcx,[142ABB060h]
  0000000140999078: mov         qword ptr [rsp+78h],rcx
  000000014099907D: mov         qword ptr [rsp+80h],26h
  0000000140999089: lea         rdx,[142ABB028h]
  0000000140999090: mov         qword ptr [rsp+90h],rdx
  0000000140999098: mov         qword ptr [rsp+98h],2
  00000001409990A4: lea         rdx,[rsp+0C0h]
  00000001409990AC: mov         qword ptr [rsp+0A0h],rdx
  00000001409990B4: mov         qword ptr [rsp+0A8h],1
  00000001409990C0: mov         qword ptr [rsp+0B0h],0
  00000001409990CC: mov         qword ptr [rsp+40h],0
  00000001409990D5: mov         qword ptr [rsp+48h],rcx
  00000001409990DA: mov         qword ptr [rsp+50h],26h
  00000001409990E3: mov         qword ptr [rsp+58h],0
  00000001409990EC: movdqu      xmmword ptr [rsp+60h],xmm0
  00000001409990F2: mov         dword ptr [rsp+88h],1
  00000001409990FD: mov         dword ptr [rsp+8Ch],eax
  0000000140999104: lea         rcx,[rsp+37h]
  0000000140999109: lea         rdx,[rsp+40h]
  000000014099910E: call        _ZN61_$LT$log..__private_api..GlobalLogger$u20$as$u20$log..Log$GT$3log17h234cc9ac9918612cE
  0000000140999113: jmp         00000001409992BD
  0000000140999118: cmp         qword ptr [rcx+r10-290h],2
  0000000140999121: jb          00000001409992FB
  0000000140999127: movss       xmm0,dword ptr [rcx+r10-270h]
  0000000140999131: pxor        xmm1,xmm1
  0000000140999135: ucomiss     xmm0,xmm1
  0000000140999138: jbe         00000001409992FB
  000000014099913E: lea         r12,[rcx+r10]
  0000000140999142: movss       xmm0,dword ptr [__real@3c23d70a]
  000000014099914A: divss       xmm0,dword ptr [r12-270h]
  0000000140999154: movaps      xmm1,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014099915B: xorps       xmm1,xmm0
  000000014099915E: ucomiss     xmm8,xmm1
  0000000140999162: jbe         00000001409992BD
  0000000140999168: addss       xmm0,dword ptr [__real@3f800000]
  0000000140999170: ucomiss     xmm0,xmm8
  0000000140999174: jbe         00000001409992BD
  000000014099917A: lea         rax,[rcx+r10]
  000000014099917E: add         rax,0FFFFFFFFFFFFFD60h
  0000000140999184: mov         qword ptr [rsp+40h],r15
  0000000140999189: cmp         qword ptr [rdi+18h],0
  000000014099918E: je          00000001409992BF
  0000000140999194: mov         r14,rax
  0000000140999197: lea         rcx,[rdi+20h]
  000000014099919B: lea         rdx,[rsp+40h]
  00000001409991A0: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  00000001409991A5: mov         rcx,qword ptr [rdi]
  00000001409991A8: mov         rdx,qword ptr [rdi+8]
  00000001409991AC: mov         r8,rdx
  00000001409991AF: and         r8,rax
  00000001409991B2: shr         rax,39h
  00000001409991B6: movd        xmm0,eax
  00000001409991BA: punpcklbw   xmm0,xmm0
  00000001409991BE: pshuflw     xmm0,xmm0,0
  00000001409991C3: pshufd      xmm0,xmm0,0
  00000001409991C8: lea         r9,[rcx-28h]
  00000001409991CC: xor         r10d,r10d
  00000001409991CF: pcmpeqd     xmm1,xmm1
  00000001409991D3: movdqu      xmm2,xmmword ptr [rcx+r8]
  00000001409991D9: movdqa      xmm3,xmm2
  00000001409991DD: pcmpeqb     xmm3,xmm0
  00000001409991E1: pmovmskb    eax,xmm3
  00000001409991E5: test        eax,eax
  00000001409991E7: je          000000014099920E
  00000001409991E9: tzcnt       r11d,eax
  00000001409991EE: add         r11,r8
  00000001409991F1: and         r11,rdx
  00000001409991F4: neg         r11
  00000001409991F7: lea         r11,[r11+r11*4]
  00000001409991FB: cmp         qword ptr [r9+r11*8],r15
  00000001409991FF: je          0000000140999231
  0000000140999201: lea         r11d,[rax-1]
  0000000140999205: and         r11w,ax
  0000000140999209: mov         eax,r11d
  000000014099920C: jne         00000001409991E9
  000000014099920E: pcmpeqb     xmm2,xmm1
  0000000140999212: pmovmskb    eax,xmm2
  0000000140999216: test        eax,eax
  0000000140999218: mov         rax,r14
  000000014099921B: jne         00000001409992BF
  0000000140999221: add         r8,r10
  0000000140999224: add         r8,10h
  0000000140999228: add         r10,10h
  000000014099922C: and         r8,rdx
  000000014099922F: jmp         00000001409991D3
  0000000140999231: mov         r15,qword ptr [rcx+r11*8-10h]
  0000000140999236: test        r15,r15
  0000000140999239: je          00000001409992B8
  000000014099923B: mov         rdi,qword ptr [rcx+r11*8-18h]
  0000000140999240: mov         rcx,rbx
  0000000140999243: movaps      xmm1,xmm6
  0000000140999246: movaps      xmm2,xmm7
  0000000140999249: call        _ZN12country_core7systems4wall10wall_state17wall_spatial_hash11WallSegment32terrain_height_closest_to_ws_pos17h51f7c99b290bdf59E
  000000014099924E: movdqa      xmm1,xmm0
  0000000140999252: mulss       xmm8,dword ptr [r12-14h]
  0000000140999259: addss       xmm1,dword ptr [rsi+4]
  000000014099925E: movaps      xmm0,xmm8
  0000000140999262: call        _ZN101_$LT$$LP$$RP$$u20$as$u20$country_core..resources..walls..decorator_storage..DecoAddrChangeTracker$GT$6insert17h37bcd7d6ada1fd09E
  0000000140999267: movaps      xmm6,xmm0
  000000014099926A: movaps      xmm7,xmm1
  000000014099926D: shl         r15,2
  0000000140999271: lea         rbx,[r15+r15*4]
  0000000140999275: movaps      xmm8,xmmword ptr [__xmm@3dcccccd3dcccccdbdcccccdbdcccccd]
  000000014099927D: lea         rsi,[rsp+40h]
  0000000140999282: xor         r15d,r15d
  0000000140999285: movups      xmm0,xmmword ptr [rdi+r15]
  000000014099928A: movaps      xmmword ptr [rsp+40h],xmm0
  000000014099928F: movaps      xmm0,xmmword ptr [rsp+40h]
  0000000140999294: addps       xmm0,xmm8
  0000000140999298: movaps      xmmword ptr [rsp+40h],xmm0
  000000014099929D: mov         rcx,rsi
  00000001409992A0: movaps      xmm1,xmm6
  00000001409992A3: movaps      xmm2,xmm7
  00000001409992A6: call        _ZN5utils8geometry4aabb5Aabb28contains17h257c0faa2f9f21ebE
  00000001409992AB: test        al,al
  00000001409992AD: jne         00000001409992BD
  00000001409992AF: add         r15,14h
  00000001409992B3: cmp         rbx,r15
  00000001409992B6: jne         0000000140999285
  00000001409992B8: mov         rax,r14
  00000001409992BB: jmp         00000001409992BF
  00000001409992BD: xor         eax,eax
  00000001409992BF: movaps      xmm6,xmmword ptr [rsp+0D0h]
  00000001409992C7: movaps      xmm7,xmmword ptr [rsp+0E0h]
  00000001409992CF: movaps      xmm8,xmmword ptr [rsp+0F0h]
  00000001409992D8: movaps      xmm9,xmmword ptr [rsp+100h]
  00000001409992E1: movaps      xmm10,xmmword ptr [rsp+110h]
  00000001409992EA: add         rsp,128h
  00000001409992F1: pop         rbx
  00000001409992F2: pop         rdi
  00000001409992F3: pop         rsi
  00000001409992F4: pop         r12
  00000001409992F6: pop         r14
  00000001409992F8: pop         r15
  00000001409992FA: ret
  00000001409992FB: lea         rcx,[anon.8b73eb39c06afd88b250ecc24c523fbb.44.llvm.11914376241322277501]
  0000000140999302: lea         r8,[142ABAFD8h]
  0000000140999309: mov         edx,32h
  000000014099930E: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  0000000140999313: lea         r8,[142ABAFC0h]
  000000014099931A: mov         rdx,r15
  000000014099931D: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140999322: int         3
  0000000140999323: CC CC CC CC CC CC CC CC CC CC CC CC CC           .............

; ===== _ZN12country_core9resources3ivy10wall_coord9WallCoord16into_world_space17h40661c77cf2d7316E:
  0000000140B199E0: push        r15
  0000000140B199E2: push        r14
  0000000140B199E4: push        r12
  0000000140B199E6: push        rsi
  0000000140B199E7: push        rdi
  0000000140B199E8: push        rbx
  0000000140B199E9: sub         rsp,0B8h
  0000000140B199F0: movaps      xmmword ptr [rsp+0A0h],xmm8
  0000000140B199F9: movaps      xmmword ptr [rsp+90h],xmm7
  0000000140B19A01: movaps      xmmword ptr [rsp+80h],xmm6
  0000000140B19A09: mov         r14,rdx
  0000000140B19A0C: movss       xmm0,dword ptr [r8+30h]
  0000000140B19A12: movss       dword ptr [rsp+2Ch],xmm0
  0000000140B19A18: movss       xmm6,dword ptr [rdx]
  0000000140B19A1C: ucomiss     xmm0,xmm6
  0000000140B19A1F: jb          0000000140B19BD7
  0000000140B19A25: xorps       xmm1,xmm1
  0000000140B19A28: ucomiss     xmm0,xmm1
  0000000140B19A2B: jne         0000000140B19A33
  0000000140B19A2D: jnp         0000000140B19C39
  0000000140B19A33: divss       xmm6,xmm0
  0000000140B19A37: movss       dword ptr [rsp+20h],xmm6
  0000000140B19A3D: ucomiss     xmm6,xmm1
  0000000140B19A40: jb          0000000140B19C51
  0000000140B19A46: movss       xmm7,dword ptr [__real@3f800000]
  0000000140B19A4E: ucomiss     xmm7,xmm6
  0000000140B19A51: jb          0000000140B19C51
  0000000140B19A57: ucomiss     xmm6,xmm6
  0000000140B19A5A: jp          0000000140B19CA9
  0000000140B19A60: mov         ebx,r9d
  0000000140B19A63: mov         rdi,r8
  0000000140B19A66: mov         rsi,rcx
  0000000140B19A69: mov         rcx,r8
  0000000140B19A6C: movaps      xmm1,xmm6
  0000000140B19A6F: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  0000000140B19A74: mov         ecx,eax
  0000000140B19A76: mov         r15,qword ptr [rdi+10h]
  0000000140B19A7A: cmp         r15,rcx
  0000000140B19A7D: jbe         0000000140B19CC1
  0000000140B19A83: lea         rax,[rcx+1]
  0000000140B19A87: cmp         rax,r15
  0000000140B19A8A: jae         0000000140B19CD0
  0000000140B19A90: mov         r12,qword ptr [rdi+8]
  0000000140B19A94: movaps      xmm1,xmm7
  0000000140B19A97: subss       xmm1,xmm0
  0000000140B19A9B: movsd       xmm2,mmword ptr [r12+rcx*8]
  0000000140B19AA1: movsd       xmm3,mmword ptr [r12+rcx*8+8]
  0000000140B19AA8: movsldup    xmm1,xmm1
  0000000140B19AAC: mulps       xmm1,xmm2
  0000000140B19AAF: movsldup    xmm0,xmm0
  0000000140B19AB3: mulps       xmm0,xmm3
  0000000140B19AB6: addps       xmm0,xmm1
  0000000140B19AB9: movlps      qword ptr [rsp+50h],xmm0
  0000000140B19ABE: lea         rcx,[rsp+30h]
  0000000140B19AC3: lea         rdx,[rsp+50h]
  0000000140B19AC8: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  0000000140B19ACD: movss       xmm8,dword ptr [r14+4]
  0000000140B19AD3: mov         rcx,rdi
  0000000140B19AD6: movaps      xmm1,xmm6
  0000000140B19AD9: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  0000000140B19ADE: mov         eax,eax
  0000000140B19AE0: lea         rcx,[rax+1]
  0000000140B19AE4: cmp         rcx,r15
  0000000140B19AE7: jae         0000000140B19CE2
  0000000140B19AED: movsd       xmm0,mmword ptr [r12+rax*8]
  0000000140B19AF3: movsd       xmm1,mmword ptr [r12+rax*8+8]
  0000000140B19AFA: subps       xmm1,xmm0
  0000000140B19AFD: movshdup    xmm0,xmm1
  0000000140B19B01: xorps       xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140B19B08: unpcklps    xmm0,xmm1
  0000000140B19B0B: mulps       xmm1,xmm1
  0000000140B19B0E: movshdup    xmm2,xmm1
  0000000140B19B12: addss       xmm2,xmm1
  0000000140B19B16: xorps       xmm1,xmm1
  0000000140B19B19: sqrtss      xmm1,xmm2
  0000000140B19B1D: divss       xmm7,xmm1
  0000000140B19B21: movsldup    xmm1,xmm7
  0000000140B19B25: mulps       xmm0,xmm1
  0000000140B19B28: movlps      qword ptr [rsp+20h],xmm0
  0000000140B19B2D: lea         rcx,[rsp+50h]
  0000000140B19B32: lea         rdx,[rsp+20h]
  0000000140B19B37: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  0000000140B19B3C: movss       xmm0,dword ptr [rsp+58h]
  0000000140B19B42: movsd       xmm1,mmword ptr [rsp+50h]
  0000000140B19B48: mulps       xmm1,xmmword ptr [__xmm@00000000000000003f2147ae3f2147ae]
  0000000140B19B4F: mulss       xmm0,dword ptr [__real@3f2147ae]
  0000000140B19B57: mulps       xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  0000000140B19B5E: mulss       xmm0,dword ptr [__real@3f000000]
  0000000140B19B66: movss       xmm3,dword ptr [rsp+30h]
  0000000140B19B6C: movss       xmm2,dword ptr [rsp+38h]
  0000000140B19B72: unpcklps    xmm3,xmm8
  0000000140B19B76: test        bl,bl
  0000000140B19B78: je          0000000140B19B89
  0000000140B19B7A: subps       xmm3,xmm1
  0000000140B19B7D: subss       xmm2,xmm0
  0000000140B19B81: movaps      xmm0,xmm2
  0000000140B19B84: movaps      xmm1,xmm3
  0000000140B19B87: jmp         0000000140B19B90
  0000000140B19B89: addps       xmm1,xmm3
  0000000140B19B8C: addss       xmm0,xmm2
  0000000140B19B90: movlps      qword ptr [rsp+30h],xmm1
  0000000140B19B95: movss       dword ptr [rsp+38h],xmm0
  0000000140B19B9B: mov         eax,dword ptr [rsp+38h]
  0000000140B19B9F: mov         dword ptr [rsi+8],eax
  0000000140B19BA2: mov         rax,qword ptr [rsp+30h]
  0000000140B19BA7: mov         qword ptr [rsi],rax
  0000000140B19BAA: mov         rax,rsi
  0000000140B19BAD: movaps      xmm6,xmmword ptr [rsp+80h]
  0000000140B19BB5: movaps      xmm7,xmmword ptr [rsp+90h]
  0000000140B19BBD: movaps      xmm8,xmmword ptr [rsp+0A0h]
  0000000140B19BC6: add         rsp,0B8h
  0000000140B19BCD: pop         rbx
  0000000140B19BCE: pop         rdi
  0000000140B19BCF: pop         rsi
  0000000140B19BD0: pop         r12
  0000000140B19BD2: pop         r14
  0000000140B19BD4: pop         r15
  0000000140B19BD6: ret
  0000000140B19BD7: mov         qword ptr [rsp+30h],r14
  0000000140B19BDC: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  0000000140B19BE3: mov         qword ptr [rsp+38h],rax
  0000000140B19BE8: lea         rcx,[rsp+2Ch]
  0000000140B19BED: mov         qword ptr [rsp+40h],rcx
  0000000140B19BF2: mov         qword ptr [rsp+48h],rax
  0000000140B19BF7: lea         rax,[anon.de362371e148dc229cb5b98a4b409393.21.llvm.14815698863116884270]
  0000000140B19BFE: mov         qword ptr [rsp+50h],rax
  0000000140B19C03: mov         qword ptr [rsp+58h],2
  0000000140B19C0C: mov         qword ptr [rsp+70h],0
  0000000140B19C15: lea         rax,[rsp+30h]
  0000000140B19C1A: mov         qword ptr [rsp+60h],rax
  0000000140B19C1F: mov         qword ptr [rsp+68h],2
  0000000140B19C28: lea         rdx,[anon.de362371e148dc229cb5b98a4b409393.23.llvm.14815698863116884270]
  0000000140B19C2F: lea         rcx,[rsp+50h]
  0000000140B19C34: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  0000000140B19C39: lea         rcx,[anon.de362371e148dc229cb5b98a4b409393.24.llvm.14815698863116884270]
  0000000140B19C40: lea         r8,[anon.de362371e148dc229cb5b98a4b409393.25.llvm.14815698863116884270]
  0000000140B19C47: mov         edx,25h
  0000000140B19C4C: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  0000000140B19C51: lea         rax,[rsp+20h]
  0000000140B19C56: mov         qword ptr [rsp+30h],rax
  0000000140B19C5B: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  0000000140B19C62: mov         qword ptr [rsp+38h],rax
  0000000140B19C67: lea         rax,[anon.12ce4fde5425384d5f438f5122902e1a.13.llvm.3277990281340110158]
  0000000140B19C6E: mov         qword ptr [rsp+50h],rax
  0000000140B19C73: mov         qword ptr [rsp+58h],1
  0000000140B19C7C: mov         qword ptr [rsp+70h],0
  0000000140B19C85: lea         rax,[rsp+30h]
  0000000140B19C8A: mov         qword ptr [rsp+60h],rax
  0000000140B19C8F: mov         qword ptr [rsp+68h],1
  0000000140B19C98: lea         rdx,[anon.de362371e148dc229cb5b98a4b409393.26.llvm.14815698863116884270]
  0000000140B19C9F: lea         rcx,[rsp+50h]
  0000000140B19CA4: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  0000000140B19CA9: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.14.llvm.3277990281340110158]
  0000000140B19CB0: lea         r8,[anon.de362371e148dc229cb5b98a4b409393.26.llvm.14815698863116884270]
  0000000140B19CB7: mov         edx,1Dh
  0000000140B19CBC: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  0000000140B19CC1: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.42.llvm.12803187488448158381]
  0000000140B19CC8: mov         rdx,r15
  0000000140B19CCB: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140B19CD0: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.43.llvm.12803187488448158381]
  0000000140B19CD7: mov         rcx,rax
  0000000140B19CDA: mov         rdx,r15
  0000000140B19CDD: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140B19CE2: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.44.llvm.12803187488448158381]
  0000000140B19CE9: mov         rdx,r15
  0000000140B19CEC: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  0000000140B19CF1: int         3
  0000000140B19CF2: CC CC CC CC CC CC CC CC CC CC CC CC CC CC        ..............

; ===== _ZN117_$LT$country_core..resources..ivy..ivy_direction_proposer..IvyDirectionProposer$u20$as$u20$core..default..Default$GT$7default17h0083e302343c7f93E:
  00000001412343B0: push        rbp
  00000001412343B1: push        rsi
  00000001412343B2: push        rdi
  00000001412343B3: sub         rsp,120h
  00000001412343BA: lea         rbp,[rsp+80h]
  00000001412343C2: mov         qword ptr [rbp+98h],0FFFFFFFFFFFFFFFEh
  00000001412343CD: mov         rsi,rcx
  00000001412343D0: lea         rdi,[rbp-58h]
  00000001412343D4: mov         edx,2Dh
  00000001412343D9: mov         rcx,rdi
  00000001412343DC: call        _ZN13bracket_noise9fastnoise9FastNoise6seeded17h69a3bd0965ddd527E
  00000001412343E1: mov         byte ptr [rbp+18h],5
  00000001412343E5: mov         byte ptr [rbp+15h],0
  00000001412343E9: mov         rcx,rdi
  00000001412343EC: mov         edx,2
  00000001412343F1: call        _ZN13bracket_noise9fastnoise9FastNoise19set_fractal_octaves17h3082a41683ec6576E
  00000001412343F6: lea         rcx,[rbp-58h]
  00000001412343FA: movss       xmm1,dword ptr [__real@40000000]
  0000000141234402: call        _ZN13bracket_noise9fastnoise9FastNoise16set_fractal_gain17h682b601aa9f95f30E
  0000000141234407: mov         dword ptr [rbp],40000000h
  000000014123440E: mov         dword ptr [rbp-8],3F800000h
  0000000141234415: lea         rcx,[rbp-58h]
  0000000141234419: mov         edx,0Dh
  000000014123441E: call        _ZN13bracket_noise9fastnoise9FastNoise8set_seed17h1ce732e59fe4620fE
  0000000141234423: lea         rcx,[rbp+20h]
  0000000141234427: mov         edx,2Dh
  000000014123442C: call        _ZN13bracket_noise9fastnoise9FastNoise6seeded17h69a3bd0965ddd527E
  0000000141234431: mov         byte ptr [rbp+90h],5
  0000000141234438: mov         byte ptr [rbp+8Dh],0
  000000014123443F: lea         rcx,[rbp+20h]
  0000000141234443: mov         edx,3
  0000000141234448: call        _ZN13bracket_noise9fastnoise9FastNoise19set_fractal_octaves17h3082a41683ec6576E
  000000014123444D: lea         rcx,[rbp+20h]
  0000000141234451: movss       xmm1,dword ptr [__real@3f800000]
  0000000141234459: call        _ZN13bracket_noise9fastnoise9FastNoise16set_fractal_gain17h682b601aa9f95f30E
  000000014123445E: mov         dword ptr [rbp+78h],3FA66666h
  0000000141234465: mov         dword ptr [rbp+70h],40000000h
  000000014123446C: lea         rcx,[rbp+20h]
  0000000141234470: mov         edx,67h
  0000000141234475: call        _ZN13bracket_noise9fastnoise9FastNoise8set_seed17h1ce732e59fe4620fE
  000000014123447A: mov         rax,qword ptr [rbp+18h]
  000000014123447E: mov         qword ptr [rsi+70h],rax
  0000000141234482: movups      xmm0,xmmword ptr [rbp+8]
  0000000141234486: movups      xmmword ptr [rsi+60h],xmm0
  000000014123448A: movups      xmm0,xmmword ptr [rbp-8]
  000000014123448E: movups      xmmword ptr [rsi+50h],xmm0
  0000000141234492: movups      xmm0,xmmword ptr [rbp-18h]
  0000000141234496: movups      xmmword ptr [rsi+40h],xmm0
  000000014123449A: movups      xmm0,xmmword ptr [rbp-58h]
  000000014123449E: movups      xmm1,xmmword ptr [rbp-48h]
  00000001412344A2: movups      xmm2,xmmword ptr [rbp-38h]
  00000001412344A6: movups      xmm3,xmmword ptr [rbp-28h]
  00000001412344AA: movups      xmmword ptr [rsi+30h],xmm3
  00000001412344AE: movups      xmmword ptr [rsi+20h],xmm2
  00000001412344B2: movups      xmmword ptr [rsi+10h],xmm1
  00000001412344B6: movups      xmmword ptr [rsi],xmm0
  00000001412344B9: movups      xmm0,xmmword ptr [rbp+20h]
  00000001412344BD: movups      xmm1,xmmword ptr [rbp+30h]
  00000001412344C1: movups      xmm2,xmmword ptr [rbp+40h]
  00000001412344C5: movups      xmm3,xmmword ptr [rbp+50h]
  00000001412344C9: movups      xmmword ptr [rsi+78h],xmm0
  00000001412344CD: movups      xmmword ptr [rsi+88h],xmm1
  00000001412344D4: movups      xmmword ptr [rsi+98h],xmm2
  00000001412344DB: movups      xmmword ptr [rsi+0A8h],xmm3
  00000001412344E2: movups      xmm0,xmmword ptr [rbp+60h]
  00000001412344E6: movups      xmmword ptr [rsi+0B8h],xmm0
  00000001412344ED: movups      xmm0,xmmword ptr [rbp+70h]
  00000001412344F1: movups      xmmword ptr [rsi+0C8h],xmm0
  00000001412344F8: movups      xmm0,xmmword ptr [rbp+80h]
  00000001412344FF: movups      xmmword ptr [rsi+0D8h],xmm0
  0000000141234506: mov         rax,qword ptr [rbp+90h]
  000000014123450D: mov         qword ptr [rsi+0E8h],rax
  0000000141234514: mov         rax,rsi
  0000000141234517: add         rsp,120h
  000000014123451E: pop         rdi
  000000014123451F: pop         rsi
  0000000141234520: pop         rbp
  0000000141234521: ret
  0000000141234522: nop         word ptr cs:[rax+rax]
  0000000141234530: mov         qword ptr [rsp+10h],rdx
  0000000141234535: push        rbp
  0000000141234536: push        rsi
  0000000141234537: push        rdi
  0000000141234538: sub         rsp,20h
  000000014123453C: lea         rbp,[rdx+80h]
  0000000141234543: lea         rcx,[rbp-58h]
  0000000141234547: call        _ZN4core3ptr363drop_in_place$LT$easy_parallel..Parallel$LT$tg_package_integrity..verify_package_manifest..CheckOutcome$GT$..each$LT$$LP$alloc..string..String$C$alloc..string..String$RP$$C$alloc..vec..Vec$LT$$LP$alloc..string..String$C$alloc..string..String$RP$$GT$$C$tg_package_integrity..verify_package_manifest..$u7b$$u7b$closure$u7d$$u7d$$GT$..$u7b$$u7b$closure$u7d$$u7d$$GT$17h2a43bffe422e9b05E.llvm.3801879985562013217
  000000014123454C: nop
  000000014123454D: add         rsp,20h
  0000000141234551: pop         rdi
  0000000141234552: pop         rsi
  0000000141234553: pop         rbp
  0000000141234554: ret
  0000000141234555: nop         word ptr cs:[rax+rax]
  0000000141234560: mov         qword ptr [rsp+10h],rdx
  0000000141234565: push        rbp
  0000000141234566: push        rsi
  0000000141234567: push        rdi
  0000000141234568: sub         rsp,20h
  000000014123456C: lea         rbp,[rdx+80h]
  0000000141234573: lea         rcx,[rbp+20h]
  0000000141234577: call        _ZN4core3ptr363drop_in_place$LT$easy_parallel..Parallel$LT$tg_package_integrity..verify_package_manifest..CheckOutcome$GT$..each$LT$$LP$alloc..string..String$C$alloc..string..String$RP$$C$alloc..vec..Vec$LT$$LP$alloc..string..String$C$alloc..string..String$RP$$GT$$C$tg_package_integrity..verify_package_manifest..$u7b$$u7b$closure$u7d$$u7d$$GT$..$u7b$$u7b$closure$u7d$$u7d$$GT$17h2a43bffe422e9b05E.llvm.3801879985562013217
  000000014123457C: nop
  000000014123457D: add         rsp,20h
  0000000141234581: pop         rdi
  0000000141234582: pop         rsi
  0000000141234583: pop         rbp
  0000000141234584: ret
  0000000141234585: CC CC CC CC CC CC CC CC CC CC CC                 ...........

; ===== _ZN12country_core9resources3ivy22ivy_direction_proposer20IvyDirectionProposer13get_direction17h3add4d225aa1f4efE:
  0000000141234590: push        rsi
  0000000141234591: push        rdi
  0000000141234592: sub         rsp,0A8h
  0000000141234599: movaps      xmmword ptr [rsp+90h],xmm12
  00000001412345A2: movaps      xmmword ptr [rsp+80h],xmm11
  00000001412345AB: movaps      xmmword ptr [rsp+70h],xmm10
  00000001412345B1: movaps      xmmword ptr [rsp+60h],xmm9
  00000001412345B7: movaps      xmmword ptr [rsp+50h],xmm8
  00000001412345BD: movaps      xmmword ptr [rsp+40h],xmm7
  00000001412345C2: movaps      xmmword ptr [rsp+30h],xmm6
  00000001412345C7: mov         rdi,r8
  00000001412345CA: mov         rsi,rcx
  00000001412345CD: movss       xmm7,dword ptr [rdx]
  00000001412345D1: movss       xmm8,dword ptr [rdx+4]
  00000001412345D7: movss       xmm0,dword ptr [__real@40333333]
  00000001412345DF: divss       xmm7,xmm0
  00000001412345E3: divss       xmm8,xmm0
  00000001412345E8: movss       xmm9,dword ptr [rdx+8]
  00000001412345EE: divss       xmm9,xmm0
  00000001412345F3: movss       xmm1,dword ptr [__real@c1b80000]
  00000001412345FB: addss       xmm1,xmm7
  00000001412345FF: movss       xmm2,dword ptr [__real@c2200000]
  0000000141234607: addss       xmm2,xmm8
  000000014123460C: movss       xmm3,dword ptr [__real@c2020000]
  0000000141234614: addss       xmm3,xmm9
  0000000141234619: call        _ZN13bracket_noise9fastnoise9FastNoise11get_noise3d17h51bbfa411866e327E
  000000014123461E: movss       xmm1,dword ptr [__real@40400000]
  0000000141234626: mulss       xmm1,xmm8
  000000014123462B: movss       xmm6,dword ptr [__real@3f800000]
  0000000141234633: movaps      xmm2,xmm6
  0000000141234636: maxss       xmm2,xmm1
  000000014123463A: movss       xmm11,dword ptr [__real@41000000]
  0000000141234643: minss       xmm11,xmm2
  0000000141234648: mulss       xmm11,xmm0
  000000014123464D: movss       xmm1,dword ptr [__real@41407ae1]
  0000000141234655: addss       xmm1,xmm7
  0000000141234659: movss       xmm2,dword ptr [__real@42860000]
  0000000141234661: addss       xmm2,xmm8
  0000000141234666: movss       xmm3,dword ptr [__real@44444000]
  000000014123466E: addss       xmm3,xmm9
  0000000141234673: mov         rcx,rsi
  0000000141234676: call        _ZN13bracket_noise9fastnoise9FastNoise11get_noise3d17h51bbfa411866e327E
  000000014123467B: movaps      xmm10,xmm0
  000000014123467F: movss       xmm1,dword ptr [__real@c1407ae1]
  0000000141234687: addss       xmm1,xmm7
  000000014123468B: movss       xmm2,dword ptr [__real@bf51eb85]
  0000000141234693: addss       xmm2,xmm8
  0000000141234698: movss       xmm3,dword ptr [__real@c4444000]
  00000001412346A0: addss       xmm3,xmm9
  00000001412346A5: mov         rcx,rsi
  00000001412346A8: call        _ZN13bracket_noise9fastnoise9FastNoise11get_noise3d17h51bbfa411866e327E
  00000001412346AD: movaps      xmm1,xmm6
  00000001412346B0: subss       xmm1,xmm0
  00000001412346B4: mulss       xmm1,dword ptr [__real@3eb33333]
  00000001412346BC: mulss       xmm0,dword ptr [__real@3f19999a]
  00000001412346C4: addss       xmm0,xmm1
  00000001412346C8: addss       xmm0,xmm10
  00000001412346CD: movaps      xmm1,xmm11
  00000001412346D1: mulss       xmm1,xmm11
  00000001412346D6: movaps      xmm2,xmm0
  00000001412346D9: mulss       xmm2,xmm0
  00000001412346DD: addss       xmm2,xmm1
  00000001412346E1: xorps       xmm1,xmm1
  00000001412346E4: sqrtss      xmm1,xmm2
  00000001412346E8: movaps      xmm10,xmm6
  00000001412346EC: divss       xmm10,xmm1
  00000001412346F1: mulss       xmm11,xmm10
  00000001412346F6: mulss       xmm10,xmm0
  00000001412346FB: lea         rax,[rdi*2+1]
  0000000141234703: shr         rdi,3Fh
  0000000141234707: mov         qword ptr [rsp+28h],rdi
  000000014123470C: mov         qword ptr [rsp+20h],rax
  0000000141234711: add         rsi,78h
  0000000141234715: addss       xmm7,dword ptr [__real@c1c00000]
  000000014123471D: lea         rdi,[rsp+20h]
  0000000141234722: mov         rcx,rdi
  0000000141234725: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  000000014123472A: movss       xmm12,dword ptr [__real@42c80000]
  0000000141234733: mulss       xmm0,xmm12
  0000000141234738: subss       xmm7,xmm0
  000000014123473C: addss       xmm8,dword ptr [__real@40e00000]
  0000000141234745: mov         rcx,rdi
  0000000141234748: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  000000014123474D: mulss       xmm0,xmm12
  0000000141234752: addss       xmm8,xmm0
  0000000141234757: addss       xmm9,dword ptr [__real@40b00000]
  0000000141234760: mov         rcx,rdi
  0000000141234763: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  0000000141234768: mulss       xmm0,xmm12
  000000014123476D: subss       xmm9,xmm0
  0000000141234772: mov         rcx,rsi
  0000000141234775: movaps      xmm1,xmm7
  0000000141234778: movaps      xmm2,xmm8
  000000014123477C: movaps      xmm3,xmm9
  0000000141234780: call        _ZN13bracket_noise9fastnoise9FastNoise11get_noise3d17h51bbfa411866e327E
  0000000141234785: addss       xmm0,xmm0
  0000000141234789: addss       xmm0,xmm6
  000000014123478D: movss       xmm1,dword ptr [__real@3f000000]
  0000000141234795: mulss       xmm0,xmm1
  0000000141234799: xorps       xmm2,xmm2
  000000014123479C: maxss       xmm2,xmm0
  00000001412347A0: movaps      xmm8,xmm6
  00000001412347A4: minss       xmm8,xmm2
  00000001412347A9: movaps      xmm0,xmm6
  00000001412347AC: subss       xmm0,xmm8
  00000001412347B1: movss       xmm2,dword ptr [__real@42a00000]
  00000001412347B9: mulss       xmm0,xmm2
  00000001412347BD: mulss       xmm8,xmm2
  00000001412347C2: subss       xmm8,xmm0
  00000001412347C7: mulss       xmm8,dword ptr [__real@3c8efa35]
  00000001412347D0: mulss       xmm8,xmm1
  00000001412347D5: movaps      xmm0,xmm8
  00000001412347D9: call        sinf
  00000001412347DE: movaps      xmm7,xmm0
  00000001412347E1: movaps      xmm0,xmm8
  00000001412347E5: call        cosf
  00000001412347EA: movaps      xmm1,xmm0
  00000001412347ED: movq        xmm0,xmm7
  00000001412347F1: xorps       xmm2,xmm2
  00000001412347F4: xorps       xmm3,xmm3
  00000001412347F7: shufps      xmm3,xmm0,84h
  00000001412347FB: movlhps     xmm1,xmm7
  00000001412347FE: xorps       xmm0,xmm0
  0000000141234801: shufps      xmm0,xmm1,24h
  0000000141234805: xorps       xmm1,xmm1
  0000000141234808: movss       xmm1,xmm11
  000000014123480D: movlhps     xmm10,xmm11
  0000000141234811: shufps      xmm10,xmm2,42h
  0000000141234816: movaps      xmm5,xmm0
  0000000141234819: mulps       xmm5,xmm0
  000000014123481C: movshdup    xmm4,xmm5
  0000000141234820: addss       xmm4,xmm5
  0000000141234824: movaps      xmm8,xmm5
  0000000141234828: unpckhpd    xmm8,xmm5
  000000014123482D: addss       xmm8,xmm4
  0000000141234832: shufps      xmm8,xmm8,0
  0000000141234837: shufps      xmm5,xmm5,0FFh
  000000014123483B: subps       xmm5,xmm8
  000000014123483F: mulps       xmm5,xmm10
  0000000141234843: movaps      xmm4,xmm10
  0000000141234847: mulps       xmm4,xmm3
  000000014123484A: movshdup    xmm8,xmm4
  000000014123484F: addss       xmm8,xmm4
  0000000141234854: movhlps     xmm4,xmm4
  0000000141234857: addss       xmm4,xmm8
  000000014123485C: addss       xmm4,xmm4
  0000000141234860: shufps      xmm4,xmm4,0
  0000000141234864: mulps       xmm4,xmm0
  0000000141234867: addps       xmm4,xmm5
  000000014123486A: movss       xmm2,xmm7
  000000014123486E: mulps       xmm2,xmm1
  0000000141234871: shufps      xmm10,xmm10,0D6h
  0000000141234876: mulps       xmm10,xmm3
  000000014123487A: subps       xmm2,xmm10
  000000014123487E: shufps      xmm2,xmm2,0E2h
  0000000141234882: addps       xmm0,xmm0
  0000000141234885: shufps      xmm0,xmm0,0FFh
  0000000141234889: mulps       xmm0,xmm2
  000000014123488C: addps       xmm0,xmm4
  000000014123488F: movshdup    xmm1,xmm0
  0000000141234893: movaps      xmm2,xmm0
  0000000141234896: mulps       xmm2,xmm0
  0000000141234899: movshdup    xmm3,xmm2
  000000014123489D: addss       xmm3,xmm2
  00000001412348A1: xorps       xmm2,xmm2
  00000001412348A4: sqrtss      xmm2,xmm3
  00000001412348A8: divss       xmm6,xmm2
  00000001412348AC: mulss       xmm0,xmm6
  00000001412348B0: mulss       xmm6,xmm1
  00000001412348B4: movaps      xmm1,xmm6
  00000001412348B7: movaps      xmm6,xmmword ptr [rsp+30h]
  00000001412348BC: movaps      xmm7,xmmword ptr [rsp+40h]
  00000001412348C1: movaps      xmm8,xmmword ptr [rsp+50h]
  00000001412348C7: movaps      xmm9,xmmword ptr [rsp+60h]
  00000001412348CD: movaps      xmm10,xmmword ptr [rsp+70h]
  00000001412348D3: movaps      xmm11,xmmword ptr [rsp+80h]
  00000001412348DC: movaps      xmm12,xmmword ptr [rsp+90h]
  00000001412348E5: add         rsp,0A8h
  00000001412348EC: pop         rdi
  00000001412348ED: pop         rsi
  00000001412348EE: ret
  00000001412348EF: CC                                               .

