
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

; ===== _ZN12country_core9resources3ivy11ivy_storage10IvyStorage9spawn_ivy17hf1a9d0b38d1a8dd5E:
  00000001408696F0: push        rbp
  00000001408696F1: push        r15
  00000001408696F3: push        r14
  00000001408696F5: push        r13
  00000001408696F7: push        r12
  00000001408696F9: push        rsi
  00000001408696FA: push        rdi
  00000001408696FB: push        rbx
  00000001408696FC: sub         rsp,198h
  0000000140869703: lea         rbp,[rsp+80h]
  000000014086970B: movaps      xmmword ptr [rbp+100h],xmm6
  0000000140869712: mov         qword ptr [rbp+0F8h],0FFFFFFFFFFFFFFFEh
  000000014086971D: movaps      xmm6,xmm3
  0000000140869720: mov         rdi,r8
  0000000140869723: mov         rbx,rdx
  0000000140869726: mov         rsi,rcx
  0000000140869729: mov         qword ptr [rbp+0D0h],0
  0000000140869734: xorps       xmm0,xmm0
  0000000140869737: movups      xmmword ptr [rbp+0E0h],xmm0
  000000014086973E: mov         qword ptr [rbp+0D8h],8
  0000000140869749: mov         byte ptr [rbp+0F7h],1
  0000000140869750: lea         rdx,[anon.65ceffb69360efd323e48f4f8756afaa.34.llvm.13316938065337964131]
  0000000140869757: lea         rcx,[rbp+0D0h]
  000000014086975E: call        _ZN5alloc11collections9vec_deque21VecDeque$LT$T$C$A$GT$4grow17h530a459ed7aeced7E
  0000000140869763: mov         rdx,qword ptr [rbp+190h]
  000000014086976A: mov         rax,qword ptr [rbp+0D0h]
  0000000140869771: mov         rcx,qword ptr [rbp+0E0h]
  0000000140869778: add         rcx,qword ptr [rbp+0E8h]
  000000014086977F: xor         r8d,r8d
  0000000140869782: cmp         rcx,rax
  0000000140869785: cmovae      r8,rax
  0000000140869789: mov         rax,qword ptr [rbp+0D8h]
  0000000140869790: sub         rcx,r8
  0000000140869793: lea         rcx,[rcx+rcx*4]
  0000000140869797: mov         qword ptr [rax+rcx*8],0
  000000014086979F: mov         r8,qword ptr [rbx]
  00000001408697A2: mov         qword ptr [rax+rcx*8+8],r8
  00000001408697A7: mov         r8d,dword ptr [rbx+8]
  00000001408697AB: mov         dword ptr [rax+rcx*8+10h],r8d
  00000001408697B0: mov         r8,qword ptr [rdi]
  00000001408697B3: mov         qword ptr [rax+rcx*8+14h],r8
  00000001408697B8: mov         r8d,dword ptr [rdi+8]
  00000001408697BC: mov         dword ptr [rax+rcx*8+1Ch],r8d
  00000001408697C1: mov         dword ptr [rax+rcx*8+20h],0
  00000001408697C9: inc         qword ptr [rbp+0E8h]
  00000001408697D0: mov         r13,qword ptr [rbp+188h]
  00000001408697D7: lea         rax,[rbp+0E0h]
  00000001408697DE: mov         r12,qword ptr [rsi+40h]
  00000001408697E2: mov         rcx,qword ptr [rbp+0D0h]
  00000001408697E9: mov         qword ptr [rbp+0C0h],rcx
  00000001408697F0: mov         rcx,qword ptr [rbp+0D8h]
  00000001408697F7: mov         qword ptr [rbp+0C8h],rcx
  00000001408697FE: movups      xmm0,xmmword ptr [rax]
  0000000140869801: movaps      xmmword ptr [rbp-60h],xmm0
  0000000140869805: sub         rdx,r13
  0000000140869808: jb          0000000140869929
  000000014086980E: lea         r15,[rsi+30h]
  0000000140869812: mov         rcx,r15
  0000000140869815: call        _ZN9perchance16PerchanceContext15usize_less_than17hdfd47e9d39dde918E
  000000014086981A: mov         r14,rax
  000000014086981D: mov         rcx,r15
  0000000140869820: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  0000000140869825: mov         rax,qword ptr [rbp+198h]
  000000014086982C: movss       xmm1,dword ptr [rbp+180h]
  0000000140869834: add         r14,r13
  0000000140869837: subss       xmm1,xmm6
  000000014086983B: mulss       xmm1,xmm0
  000000014086983F: addss       xmm1,xmm6
  0000000140869843: mov         qword ptr [rbp+50h],r12
  0000000140869847: mov         qword ptr [rbp+58h],0
  000000014086984F: mov         rcx,qword ptr [rbx]
  0000000140869852: mov         qword ptr [rbp+60h],rcx
  0000000140869856: mov         ecx,dword ptr [rbx+8]
  0000000140869859: mov         dword ptr [rbp+68h],ecx
  000000014086985C: mov         rcx,qword ptr [rdi]
  000000014086985F: mov         qword ptr [rbp+6Ch],rcx
  0000000140869863: mov         ecx,dword ptr [rdi+8]
  0000000140869866: mov         dword ptr [rbp+74h],ecx
  0000000140869869: mov         dword ptr [rbp+78h],0
  0000000140869870: mov         rcx,qword ptr [rbp+0C0h]
  0000000140869877: mov         qword ptr [rbp+30h],rcx
  000000014086987B: mov         rcx,qword ptr [rbp+0C8h]
  0000000140869882: mov         qword ptr [rbp+38h],rcx
  0000000140869886: movaps      xmm0,xmmword ptr [rbp-60h]
  000000014086988A: movups      xmmword ptr [rbp+40h],xmm0
  000000014086988E: mov         qword ptr [rbp+80h],r14
  0000000140869895: mov         dword ptr [rbp+98h],0
  000000014086989F: mov         dword ptr [rbp+0B4h],0
  00000001408698A9: movss       dword ptr [rbp+0B8h],xmm1
  00000001408698B1: mov         qword ptr [rbp+88h],1
  00000001408698BC: mov         qword ptr [rbp+90h],rax
  00000001408698C3: mov         byte ptr [rbp+0BCh],0
  00000001408698CA: mov         r8,qword ptr [rsi+40h]
  00000001408698CE: mov         byte ptr [rbp+0F7h],0
  00000001408698D5: lea         rcx,[rbp-60h]
  00000001408698D9: lea         r9,[rbp+30h]
  00000001408698DD: mov         rdx,rsi
  00000001408698E0: call        _ZN9hashbrown3map28HashMap$LT$K$C$V$C$S$C$A$GT$6insert17h573c000fd6a48f46E
  00000001408698E5: mov         rax,qword ptr [rbp-60h]
  00000001408698E9: mov         rcx,rax
  00000001408698EC: neg         rcx
  00000001408698EF: jo          000000014086990A
  00000001408698F1: jae         000000014086990A
  00000001408698F3: mov         rcx,qword ptr [rbp-58h]
  00000001408698F7: shl         rax,3
  00000001408698FB: lea         rdx,[rax+rax*4]
  00000001408698FF: mov         r8d,8
  0000000140869905: call        __rust_dealloc
  000000014086990A: inc         qword ptr [rsi+40h]
  000000014086990E: movaps      xmm6,xmmword ptr [rbp+100h]
  0000000140869915: add         rsp,198h
  000000014086991C: pop         rbx
  000000014086991D: pop         rdi
  000000014086991E: pop         rsi
  000000014086991F: pop         r12
  0000000140869921: pop         r13
  0000000140869923: pop         r14
  0000000140869925: pop         r15
  0000000140869927: pop         rbp
  0000000140869928: ret
  0000000140869929: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.33.llvm.3277990281340110158]
  0000000140869930: lea         r8,[anon.12ce4fde5425384d5f438f5122902e1a.35.llvm.3277990281340110158]
  0000000140869937: mov         edx,1Eh
  000000014086993C: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  0000000140869941: ud2
  0000000140869943: nop         word ptr cs:[rax+rax]
  0000000140869950: mov         qword ptr [rsp+10h],rdx
  0000000140869955: push        rbp
  0000000140869956: push        r15
  0000000140869958: push        r14
  000000014086995A: push        r13
  000000014086995C: push        r12
  000000014086995E: push        rsi
  000000014086995F: push        rdi
  0000000140869960: push        rbx
  0000000140869961: sub         rsp,38h
  0000000140869965: lea         rbp,[rdx+80h]
  000000014086996C: movaps      xmmword ptr [rsp+20h],xmm6
  0000000140869971: mov         rax,qword ptr [rbp+0C0h]
  0000000140869978: test        rax,rax
  000000014086997B: je          0000000140869997
  000000014086997D: shl         rax,3
  0000000140869981: lea         rdx,[rax+rax*4]
  0000000140869985: mov         r8d,8
  000000014086998B: mov         rcx,qword ptr [rbp+0C8h]
  0000000140869992: call        __rust_dealloc
  0000000140869997: mov         byte ptr [rbp+0F7h],0
  000000014086999E: movaps      xmm6,xmmword ptr [rsp+20h]
  00000001408699A3: add         rsp,38h
  00000001408699A7: pop         rbx
  00000001408699A8: pop         rdi
  00000001408699A9: pop         rsi
  00000001408699AA: pop         r12
  00000001408699AC: pop         r13
  00000001408699AE: pop         r14
  00000001408699B0: pop         r15
  00000001408699B2: pop         rbp
  00000001408699B3: ret
  00000001408699B4: nop         word ptr cs:[rax+rax]
  00000001408699C0: mov         qword ptr [rsp+10h],rdx
  00000001408699C5: push        rbp
  00000001408699C6: push        r15
  00000001408699C8: push        r14
  00000001408699CA: push        r13
  00000001408699CC: push        r12
  00000001408699CE: push        rsi
  00000001408699CF: push        rdi
  00000001408699D0: push        rbx
  00000001408699D1: sub         rsp,38h
  00000001408699D5: lea         rbp,[rdx+80h]
  00000001408699DC: movaps      xmmword ptr [rsp+20h],xmm6
  00000001408699E1: cmp         byte ptr [rbp+0F7h],0
  00000001408699E8: je          0000000140869A10
  00000001408699EA: mov         rax,qword ptr [rbp+0D0h]
  00000001408699F1: test        rax,rax
  00000001408699F4: je          0000000140869A10
  00000001408699F6: mov         rcx,qword ptr [rbp+0D8h]
  00000001408699FD: shl         rax,3
  0000000140869A01: lea         rdx,[rax+rax*4]
  0000000140869A05: mov         r8d,8
  0000000140869A0B: call        __rust_dealloc
  0000000140869A10: movaps      xmm6,xmmword ptr [rsp+20h]
  0000000140869A15: add         rsp,38h
  0000000140869A19: pop         rbx
  0000000140869A1A: pop         rdi
  0000000140869A1B: pop         rsi
  0000000140869A1C: pop         r12
  0000000140869A1E: pop         r13
  0000000140869A20: pop         r14
  0000000140869A22: pop         r15
  0000000140869A24: pop         rbp
  0000000140869A25: ret
  0000000140869A26: CC CC CC CC CC CC CC CC CC CC                    ..........

; ===== _ZN12country_core7systems3ivy11ivy_spawner11ivy_spawner17h00fc38d9e49c0473E:
  000000014094CF90: push        r15
  000000014094CF92: push        r14
  000000014094CF94: push        r13
  000000014094CF96: push        r12
  000000014094CF98: push        rsi
  000000014094CF99: push        rdi
  000000014094CF9A: push        rbp
  000000014094CF9B: push        rbx
  000000014094CF9C: sub         rsp,1D8h
  000000014094CFA3: movaps      xmmword ptr [rsp+1C0h],xmm15
  000000014094CFAC: movaps      xmmword ptr [rsp+1B0h],xmm14
  000000014094CFB5: movaps      xmmword ptr [rsp+1A0h],xmm13
  000000014094CFBE: movaps      xmmword ptr [rsp+190h],xmm12
  000000014094CFC7: movaps      xmmword ptr [rsp+180h],xmm11
  000000014094CFD0: movaps      xmmword ptr [rsp+170h],xmm10
  000000014094CFD9: movaps      xmmword ptr [rsp+160h],xmm9
  000000014094CFE2: movaps      xmmword ptr [rsp+150h],xmm8
  000000014094CFEB: movaps      xmmword ptr [rsp+140h],xmm7
  000000014094CFF3: movaps      xmmword ptr [rsp+130h],xmm6
  000000014094CFFB: mov         rax,qword ptr [rsp+250h]
  000000014094D003: mov         rax,qword ptr [rax]
  000000014094D006: cmp         byte ptr [rax+1Ch],1
  000000014094D00A: jne         000000014094DE8C
  000000014094D010: mov         r10,qword ptr [rcx]
  000000014094D013: mov         rax,qword ptr [r10+18h]
  000000014094D017: test        rax,rax
  000000014094D01A: mov         qword ptr [rsp+0A8h],r10
  000000014094D022: je          000000014094D0AC
  000000014094D028: mov         r10,qword ptr [r10]
  000000014094D02B: lea         r11,[r10+10h]
  000000014094D02F: movdqa      xmm0,xmmword ptr [r10]
  000000014094D034: pmovmskb    esi,xmm0
  000000014094D038: not         esi
  000000014094D03A: xor         ebp,ebp
  000000014094D03C: jmp         000000014094D065
  000000014094D03E: nop
  000000014094D040: lea         edi,[rsi-1]
  000000014094D043: and         edi,esi
  000000014094D045: tzcnt       esi,esi
  000000014094D049: neg         rsi
  000000014094D04C: imul        rsi,rsi,98h
  000000014094D053: mov         rsi,qword ptr [r10+rsi-40h]
  000000014094D058: add         rbp,rsi
  000000014094D05B: dec         rbp
  000000014094D05E: mov         esi,edi
  000000014094D060: dec         rax
  000000014094D063: je          000000014094D09B
  000000014094D065: test        si,si
  000000014094D068: jne         000000014094D040
  000000014094D06A: nop         word ptr [rax+rax]
  000000014094D070: movdqa      xmm0,xmmword ptr [r11]
  000000014094D075: pmovmskb    esi,xmm0
  000000014094D079: add         r10,0FFFFFFFFFFFFF680h
  000000014094D080: add         r11,10h
  000000014094D084: cmp         esi,0FFFFh
  000000014094D08A: je          000000014094D070
  000000014094D08C: mov         ebx,0FFFFFFFEh
  000000014094D091: sub         ebx,esi
  000000014094D093: not         esi
  000000014094D095: mov         edi,esi
  000000014094D097: and         edi,ebx
  000000014094D099: jmp         000000014094D045
  000000014094D09B: lea         rax,[rbp+13h]
  000000014094D09F: cmp         rax,9C40h
  000000014094D0A5: jb          000000014094D0AE
  000000014094D0A7: jmp         000000014094DE8C
  000000014094D0AC: xor         ebp,ebp
  000000014094D0AE: mov         r15,qword ptr [rsp+248h]
  000000014094D0B6: mov         rax,qword ptr [rsp+240h]
  000000014094D0BE: mov         rax,qword ptr [rax]
  000000014094D0C1: mov         r12d,dword ptr [rax+120h]
  000000014094D0C8: cmp         r12d,3
  000000014094D0CC: mov         qword ptr [rsp+68h],rcx
  000000014094D0D1: jae         000000014094D297
  000000014094D0D7: mov         rsi,r8
  000000014094D0DA: mov         r14,r9
  000000014094D0DD: mov         rax,qword ptr [r9]
  000000014094D0E0: mov         rdi,qword ptr [r15]
  000000014094D0E3: xorps       xmm6,xmm6
  000000014094D0E6: test        byte ptr [rax],1
  000000014094D0E9: je          000000014094D0F9
  000000014094D0EB: movd        xmm6,dword ptr [rax+4]
  000000014094D0F0: movd        xmm0,dword ptr [rax+0Ch]
  000000014094D0F5: punpckldq   xmm6,xmm0
  000000014094D0F9: movsd       xmm0,mmword ptr [rdi+50h]
  000000014094D0FE: movaps      xmm7,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094D105: mulps       xmm7,xmm0
  000000014094D108: addps       xmm7,xmm6
  000000014094D10B: movsd       xmm9,mmword ptr [rdi+48h]
  000000014094D111: cvtdq2ps    xmm1,xmm9
  000000014094D115: divps       xmm0,xmm1
  000000014094D118: divps       xmm7,xmm0
  000000014094D11B: movshdup    xmm0,xmm7
  000000014094D11F: call        floorf
  000000014094D124: movaps      xmm8,xmm0
  000000014094D128: movaps      xmm0,xmm7
  000000014094D12B: call        floorf
  000000014094D130: cvttss2si   eax,xmm0
  000000014094D134: movss       xmm1,dword ptr [__real@4effffff]
  000000014094D13C: ucomiss     xmm0,xmm1
  000000014094D13F: mov         edx,7FFFFFFFh
  000000014094D144: cmova       eax,edx
  000000014094D147: xor         r8d,r8d
  000000014094D14A: ucomiss     xmm0,xmm0
  000000014094D14D: cmovp       eax,r8d
  000000014094D151: cvttss2si   ecx,xmm8
  000000014094D156: ucomiss     xmm8,xmm1
  000000014094D15A: cmova       ecx,edx
  000000014094D15D: ucomiss     xmm8,xmm8
  000000014094D161: cmovp       ecx,r8d
  000000014094D165: test        eax,eax
  000000014094D167: js          000000014094DE8C
  000000014094D16D: movd        xmm0,eax
  000000014094D171: movd        xmm1,ecx
  000000014094D175: punpckldq   xmm0,xmm1
  000000014094D179: movq        xmm0,xmm0
  000000014094D17D: movaps      xmm1,xmm9
  000000014094D181: pcmpgtd     xmm1,xmm0
  000000014094D185: pshufd      xmm1,xmm1,50h
  000000014094D18A: movmskpd    ecx,xmm1
  000000014094D18E: test        cl,2
  000000014094D191: je          000000014094DE8C
  000000014094D197: test        cl,1
  000000014094D19A: je          000000014094DE8C
  000000014094D1A0: pshufd      xmm0,xmm0,55h
  000000014094D1A5: movd        ecx,xmm0
  000000014094D1A9: test        ecx,ecx
  000000014094D1AB: js          000000014094DE8C
  000000014094D1B1: movd        edx,xmm9
  000000014094D1B6: imul        ecx,edx
  000000014094D1B9: add         ecx,eax
  000000014094D1BB: movsxd      rcx,ecx
  000000014094D1BE: cmp         qword ptr [rdi+10h],rcx
  000000014094D1C2: jbe         000000014094DE8C
  000000014094D1C8: mov         rax,qword ptr [rdi+8]
  000000014094D1CC: lea         rcx,[rcx+rcx*2]
  000000014094D1D0: shl         rcx,4
  000000014094D1D4: mov         rbx,qword ptr [rax+rcx+10h]
  000000014094D1D9: test        rbx,rbx
  000000014094D1DC: je          000000014094DE8C
  000000014094D1E2: mov         rdi,qword ptr [rax+rcx+8]
  000000014094D1E7: movshdup    xmm2,xmm6
  000000014094D1EB: mov         qword ptr [rsp+20h],rbx
  000000014094D1F0: mov         dword ptr [rsp+28h],3F000000h
  000000014094D1F8: lea         rcx,[rsp+70h]
  000000014094D1FD: movaps      xmm1,xmm6
  000000014094D200: mov         r9,rdi
  000000014094D203: call        _ZN12country_core7systems3ivy10ivy_grower21closest_segment_index17hba4d5971b8993167E
  000000014094D208: cmp         dword ptr [rsp+70h],1
  000000014094D20D: jne         000000014094DE8C
  000000014094D213: mov         rcx,qword ptr [rsp+78h]
  000000014094D218: cmp         rcx,rbx
  000000014094D21B: jae         000000014094DFE3
  000000014094D221: imul        rax,rcx,38h
  000000014094D225: movsd       xmm1,mmword ptr [rdi+rax]
  000000014094D22A: movsd       xmm0,mmword ptr [rdi+rax+8]
  000000014094D230: subps       xmm6,xmm1
  000000014094D233: movaps      xmm2,xmm0
  000000014094D236: subps       xmm2,xmm1
  000000014094D239: mulps       xmm6,xmm2
  000000014094D23C: movshdup    xmm3,xmm6
  000000014094D240: mulps       xmm2,xmm2
  000000014094D243: movshdup    xmm4,xmm2
  000000014094D247: addss       xmm4,xmm2
  000000014094D24B: maxss       xmm4,dword ptr [__real@1e3ce508]
  000000014094D253: addss       xmm3,xmm6
  000000014094D257: divss       xmm3,xmm4
  000000014094D25B: xorps       xmm8,xmm8
  000000014094D25F: xorps       xmm2,xmm2
  000000014094D262: maxss       xmm2,xmm3
  000000014094D266: movss       xmm3,dword ptr [__real@3f800000]
  000000014094D26E: movaps      xmm4,xmm3
  000000014094D271: minss       xmm4,xmm2
  000000014094D275: subss       xmm3,xmm4
  000000014094D279: movsldup    xmm2,xmm3
  000000014094D27D: mulps       xmm2,xmm1
  000000014094D280: movsldup    xmm7,xmm4
  000000014094D284: mulps       xmm7,xmm0
  000000014094D287: addps       xmm7,xmm2
  000000014094D28A: mov         rdi,qword ptr [rdi+rax+28h]
  000000014094D28F: mov         rsi,qword ptr [rsi]
  000000014094D292: jmp         000000014094D49F
  000000014094D297: mov         rsi,qword ptr [r8]
  000000014094D29A: mov         eax,dword ptr [rcx+1Ch]
  000000014094D29D: mov         rcx,qword ptr [rcx+10h]
  000000014094D2A1: mov         dword ptr [rcx],eax
  000000014094D2A3: mov         r14,qword ptr [rsi+18h]
  000000014094D2A7: test        r14,r14
  000000014094D2AA: je          000000014094DE8C
  000000014094D2B0: mov         dword ptr [rsp+48h],r12d
  000000014094D2B5: mov         qword ptr [rsp+0A0h],r9
  000000014094D2BD: mov         rdi,qword ptr [rdx]
  000000014094D2C0: mov         r13,qword ptr [rsi]
  000000014094D2C3: movdqa      xmm0,xmmword ptr [r13]
  000000014094D2C9: pmovmskb    r12d,xmm0
  000000014094D2CE: not         r12d
  000000014094D2D1: lea         r15,[r13+10h]
  000000014094D2D5: mov         rax,qword ptr [rsp+0A8h]
  000000014094D2DD: lea         rbx,[rax+30h]
  000000014094D2E1: mov         rcx,rbx
  000000014094D2E4: mov         rdx,r14
  000000014094D2E7: call        _ZN9perchance16PerchanceContext15usize_less_than17hdfd47e9d39dde918E
  000000014094D2EC: test        rax,rax
  000000014094D2EF: je          000000014094D359
  000000014094D2F1: lea         rcx,[rax-1]
  000000014094D2F5: cmp         r14,rcx
  000000014094D2F8: jbe         000000014094D364
  000000014094D2FA: xor         ecx,ecx
  000000014094D2FC: jmp         000000014094D313
  000000014094D2FE: nop
  000000014094D300: lea         edx,[r12-1]
  000000014094D305: and         edx,r12d
  000000014094D308: mov         r12d,edx
  000000014094D30B: inc         rcx
  000000014094D30E: cmp         rcx,rax
  000000014094D311: je          000000014094D354
  000000014094D313: test        r12w,r12w
  000000014094D317: jne         000000014094D300
  000000014094D319: nop         dword ptr [rax]
  000000014094D320: movdqa      xmm0,xmmword ptr [r15]
  000000014094D325: pmovmskb    r12d,xmm0
  000000014094D32A: add         r13,0FFFFFFFFFFFFD580h
  000000014094D331: add         r15,10h
  000000014094D335: cmp         r12d,0FFFFh
  000000014094D33C: je          000000014094D320
  000000014094D33E: mov         edx,0FFFFFFFEh
  000000014094D343: sub         edx,r12d
  000000014094D346: not         r12d
  000000014094D349: and         r12d,edx
  000000014094D34C: inc         rcx
  000000014094D34F: cmp         rcx,rax
  000000014094D352: jne         000000014094D313
  000000014094D354: cmp         r14,rax
  000000014094D357: je          000000014094D364
  000000014094D359: test        r12w,r12w
  000000014094D35D: je          000000014094D370
  000000014094D35F: test        r13,r13
  000000014094D362: jne         000000014094D391
  000000014094D364: lea         rcx,[142AB3890h]
  000000014094D36B: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094D370: movdqa      xmm0,xmmword ptr [r15]
  000000014094D375: pmovmskb    r12d,xmm0
  000000014094D37A: add         r13,0FFFFFFFFFFFFD580h
  000000014094D381: add         r15,10h
  000000014094D385: cmp         r12d,0FFFFh
  000000014094D38C: je          000000014094D370
  000000014094D38E: not         r12d
  000000014094D391: mov         rcx,rbx
  000000014094D394: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  000000014094D399: movd        dword ptr [rsp+0B0h],xmm0
  000000014094D3A2: xorps       xmm1,xmm1
  000000014094D3A5: ucomiss     xmm0,xmm1
  000000014094D3A8: jb          000000014094DF28
  000000014094D3AE: movss       xmm6,dword ptr [__real@3f800000]
  000000014094D3B6: ucomiss     xmm6,xmm0
  000000014094D3B9: jb          000000014094DF28
  000000014094D3BF: ucomiss     xmm0,xmm0
  000000014094D3C2: jp          000000014094DF8C
  000000014094D3C8: tzcnt       eax,r12d
  000000014094D3CD: neg         rax
  000000014094D3D0: imul        rax,rax,2A8h
  000000014094D3D7: lea         rbx,[rax+r13]
  000000014094D3DB: lea         rcx,[rax+r13]
  000000014094D3DF: add         rcx,0FFFFFFFFFFFFFD60h
  000000014094D3E6: movdqa      xmm1,xmm0
  000000014094D3EA: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094D3EF: mov         ecx,eax
  000000014094D3F1: mov         rdx,qword ptr [rbx-290h]
  000000014094D3F8: cmp         rdx,rcx
  000000014094D3FB: jbe         000000014094DFBC
  000000014094D401: lea         rax,[rcx+1]
  000000014094D405: cmp         rax,rdx
  000000014094D408: mov         r15,qword ptr [rsp+248h]
  000000014094D410: mov         r12d,dword ptr [rsp+48h]
  000000014094D415: jae         000000014094DFC8
  000000014094D41B: mov         rax,qword ptr [rbx-298h]
  000000014094D422: subss       xmm6,xmm0
  000000014094D426: movsd       xmm1,mmword ptr [rax+rcx*8]
  000000014094D42B: movsd       xmm2,mmword ptr [rax+rcx*8+8]
  000000014094D431: movsldup    xmm3,xmm6
  000000014094D435: mulps       xmm3,xmm1
  000000014094D438: movsldup    xmm0,xmm0
  000000014094D43C: mulps       xmm0,xmm2
  000000014094D43F: addps       xmm0,xmm3
  000000014094D442: movlps      qword ptr [rsp+70h],xmm0
  000000014094D447: lea         rcx,[rsp+0C4h]
  000000014094D44F: lea         rdx,[rsp+70h]
  000000014094D454: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094D459: movss       xmm7,dword ptr [rsp+0C4h]
  000000014094D462: movss       xmm6,dword ptr [rsp+0CCh]
  000000014094D46B: mov         rcx,rdi
  000000014094D46E: movaps      xmm1,xmm7
  000000014094D471: movaps      xmm2,xmm6
  000000014094D474: call        _ZN13bracket_noise9fastnoise9FastNoise9get_noise17h6c2319d941a88ccaE
  000000014094D479: ucomiss     xmm0,dword ptr [rdi+78h]
  000000014094D47D: mov         r14,qword ptr [rsp+0A0h]
  000000014094D485: jbe         000000014094DE8C
  000000014094D48B: mov         rdi,qword ptr [rbx-2A8h]
  000000014094D492: movss       xmm8,dword ptr [rsp+0C8h]
  000000014094D49C: unpcklps    xmm7,xmm6
  000000014094D49F: mov         qword ptr [rsp+70h],rdi
  000000014094D4A4: cmp         qword ptr [rsi+18h],0
  000000014094D4A9: je          000000014094D680
  000000014094D4AF: mov         rbx,r14
  000000014094D4B2: lea         rcx,[rsi+20h]
  000000014094D4B6: lea         rdx,[rsp+70h]
  000000014094D4BB: call        _ZN4core4hash11BuildHasher8hash_one17h002a39f6638d7ef8E
  000000014094D4C0: mov         rcx,qword ptr [rsi]
  000000014094D4C3: mov         rdx,qword ptr [rsi+8]
  000000014094D4C7: mov         r8,rdx
  000000014094D4CA: and         r8,rax
  000000014094D4CD: shr         rax,39h
  000000014094D4D1: movd        xmm0,eax
  000000014094D4D5: punpcklbw   xmm0,xmm0
  000000014094D4D9: pshuflw     xmm0,xmm0,0
  000000014094D4DE: pshufd      xmm0,xmm0,0
  000000014094D4E3: lea         rax,[rcx-2A8h]
  000000014094D4EA: xor         r9d,r9d
  000000014094D4ED: pcmpeqd     xmm1,xmm1
  000000014094D4F1: movdqu      xmm2,xmmword ptr [rcx+r8]
  000000014094D4F7: movdqa      xmm3,xmm2
  000000014094D4FB: pcmpeqb     xmm3,xmm0
  000000014094D4FF: pmovmskb    r10d,xmm3
  000000014094D504: test        r10d,r10d
  000000014094D507: je          000000014094D531
  000000014094D509: tzcnt       r11d,r10d
  000000014094D50E: add         r11,r8
  000000014094D511: and         r11,rdx
  000000014094D514: neg         r11
  000000014094D517: imul        r11,r11,2A8h
  000000014094D51E: cmp         qword ptr [rax+r11],rdi
  000000014094D522: je          000000014094D553
  000000014094D524: lea         r11d,[r10-1]
  000000014094D528: and         r11w,r10w
  000000014094D52C: mov         r10d,r11d
  000000014094D52F: jne         000000014094D509
  000000014094D531: pcmpeqb     xmm2,xmm1
  000000014094D535: pmovmskb    r10d,xmm2
  000000014094D53A: test        r10d,r10d
  000000014094D53D: jne         000000014094D680
  000000014094D543: add         r8,r9
  000000014094D546: add         r8,10h
  000000014094D54A: add         r9,10h
  000000014094D54E: and         r8,rdx
  000000014094D551: jmp         000000014094D4F1
  000000014094D553: mov         qword ptr [rsp+0F8h],rdi
  000000014094D55B: cmp         qword ptr [rcx+r11-290h],2
  000000014094D564: jb          000000014094DFA4
  000000014094D56A: movss       xmm0,dword ptr [rcx+r11-270h]
  000000014094D574: xorps       xmm6,xmm6
  000000014094D577: ucomiss     xmm0,xmm6
  000000014094D57A: jbe         000000014094DFA4
  000000014094D580: lea         rsi,[rcx+r11]
  000000014094D584: lea         rdi,[rcx+r11]
  000000014094D588: add         rdi,0FFFFFFFFFFFFFD60h
  000000014094D58F: movshdup    xmm2,xmm7
  000000014094D593: lea         r9,[142AB3860h]
  000000014094D59A: mov         rcx,rdi
  000000014094D59D: movaps      xmm1,xmm7
  000000014094D5A0: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$21get_approx_u_from_pos17h5da41a19c706643fE
  000000014094D5A5: maxss       xmm6,xmm0
  000000014094D5A9: movss       xmm11,dword ptr [__real@3f800000]
  000000014094D5B2: movaps      xmm1,xmm11
  000000014094D5B6: minss       xmm1,xmm6
  000000014094D5BA: movss       xmm0,dword ptr [rsi-270h]
  000000014094D5C2: movss       dword ptr [rsp+0C0h],xmm0
  000000014094D5CB: mov         rcx,rdi
  000000014094D5CE: movss       dword ptr [rsp+4Ch],xmm1
  000000014094D5D4: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094D5D9: mov         eax,eax
  000000014094D5DB: lea         rcx,[rax+1]
  000000014094D5DF: mov         rdx,qword ptr [rsi-290h]
  000000014094D5E6: cmp         rcx,rdx
  000000014094D5E9: jae         000000014094DFD7
  000000014094D5EF: mov         rcx,qword ptr [rsi-298h]
  000000014094D5F6: movsd       xmm0,mmword ptr [rcx+rax*8]
  000000014094D5FB: movsd       xmm1,mmword ptr [rcx+rax*8+8]
  000000014094D601: subps       xmm1,xmm0
  000000014094D604: movshdup    xmm0,xmm1
  000000014094D608: movaps      xmm6,xmmword ptr [__xmm@80000000800000008000000080000000]
  000000014094D60F: xorps       xmm0,xmm6
  000000014094D612: unpcklps    xmm0,xmm1
  000000014094D615: mulps       xmm1,xmm1
  000000014094D618: movshdup    xmm2,xmm1
  000000014094D61C: addss       xmm2,xmm1
  000000014094D620: xorps       xmm1,xmm1
  000000014094D623: sqrtss      xmm1,xmm2
  000000014094D627: movaps      xmm2,xmm11
  000000014094D62B: divss       xmm2,xmm1
  000000014094D62F: movsldup    xmm1,xmm2
  000000014094D633: mulps       xmm0,xmm1
  000000014094D636: movlps      qword ptr [rsp+50h],xmm0
  000000014094D63B: lea         rcx,[rsp+70h]
  000000014094D640: lea         rdx,[rsp+50h]
  000000014094D645: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094D64A: movsd       xmm12,mmword ptr [rsp+70h]
  000000014094D651: movss       xmm10,dword ptr [rsp+78h]
  000000014094D658: cmp         r12d,3
  000000014094D65C: jae         000000014094D68C
  000000014094D65E: mov         rax,qword ptr [rbx]
  000000014094D661: xorps       xmm2,xmm2
  000000014094D664: test        byte ptr [rax],1
  000000014094D667: xorps       xmm1,xmm1
  000000014094D66A: je          000000014094D676
  000000014094D66C: movss       xmm1,dword ptr [rax+4]
  000000014094D671: movss       xmm2,dword ptr [rax+0Ch]
  000000014094D676: mov         rcx,rdi
  000000014094D679: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$16is_on_right_side17h9b4fcfa193d90f90E
  000000014094D67E: jmp         000000014094D6B5
  000000014094D680: lea         rcx,[142AB3848h]
  000000014094D687: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014094D68C: cmp         dword ptr [rsi-48h],2
  000000014094D690: jb          000000014094D786
  000000014094D696: mov         rax,qword ptr [rsp+68h]
  000000014094D69B: mov         ecx,dword ptr [rax+1Ch]
  000000014094D69E: mov         rax,qword ptr [rax+10h]
  000000014094D6A2: mov         dword ptr [rax],ecx
  000000014094D6A4: mov         rax,qword ptr [rsp+0A8h]
  000000014094D6AC: lea         rcx,[rax+30h]
  000000014094D6B0: call        _ZN9perchance16PerchanceContext8get_bool17he9e486d19cfaf047E
  000000014094D6B5: test        al,al
  000000014094D6B7: je          000000014094D786
  000000014094D6BD: mov         rcx,rdi
  000000014094D6C0: movd        xmm1,dword ptr [rsp+4Ch]
  000000014094D6C6: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094D6CB: mov         ecx,eax
  000000014094D6CD: mov         rdx,qword ptr [rsi-290h]
  000000014094D6D4: cmp         rdx,rcx
  000000014094D6D7: jbe         000000014094DFBC
  000000014094D6DD: lea         rax,[rcx+1]
  000000014094D6E1: cmp         rax,rdx
  000000014094D6E4: jae         000000014094DFC8
  000000014094D6EA: movaps      xmm9,xmm12
  000000014094D6EE: xorps       xmm9,xmm6
  000000014094D6F2: xorps       xmm6,xmm10
  000000014094D6F6: mov         rax,qword ptr [rsi-298h]
  000000014094D6FD: movaps      xmm1,xmm11
  000000014094D701: subss       xmm1,xmm0
  000000014094D705: movsd       xmm2,mmword ptr [rax+rcx*8]
  000000014094D70A: movsd       xmm3,mmword ptr [rax+rcx*8+8]
  000000014094D710: movsldup    xmm1,xmm1
  000000014094D714: mulps       xmm1,xmm2
  000000014094D717: movsldup    xmm0,xmm0
  000000014094D71B: mulps       xmm0,xmm3
  000000014094D71E: addps       xmm0,xmm1
  000000014094D721: movlps      qword ptr [rsp+50h],xmm0
  000000014094D726: lea         rcx,[rsp+70h]
  000000014094D72B: lea         rdx,[rsp+50h]
  000000014094D730: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094D735: mulps       xmm12,xmmword ptr [__xmm@0000000000000000bf2e147bbf2e147b]
  000000014094D73D: mulss       xmm10,dword ptr [__real@bf2e147b]
  000000014094D746: mulss       xmm10,dword ptr [__real@3f000000]
  000000014094D74F: mulps       xmm12,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094D757: movsd       xmm0,mmword ptr [rsp+70h]
  000000014094D75D: addps       xmm0,xmm12
  000000014094D761: movaps      xmmword ptr [rsp+0D0h],xmm0
  000000014094D769: addss       xmm10,dword ptr [rsp+78h]
  000000014094D770: movaps      xmmword ptr [rsp+0E0h],xmm10
  000000014094D779: movaps      xmm10,xmm6
  000000014094D77D: movaps      xmm12,xmm9
  000000014094D781: jmp         000000014094D838
  000000014094D786: mov         rcx,rdi
  000000014094D789: movd        xmm1,dword ptr [rsp+4Ch]
  000000014094D78F: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$14get_coord_at_u17h153d3a6b926e2107E
  000000014094D794: mov         ecx,eax
  000000014094D796: mov         rdx,qword ptr [rsi-290h]
  000000014094D79D: cmp         rdx,rcx
  000000014094D7A0: jbe         000000014094DFBC
  000000014094D7A6: lea         rax,[rcx+1]
  000000014094D7AA: cmp         rax,rdx
  000000014094D7AD: jae         000000014094DFC8
  000000014094D7B3: mov         rax,qword ptr [rsi-298h]
  000000014094D7BA: movaps      xmm1,xmm11
  000000014094D7BE: subss       xmm1,xmm0
  000000014094D7C2: movsd       xmm2,mmword ptr [rax+rcx*8]
  000000014094D7C7: movsd       xmm3,mmword ptr [rax+rcx*8+8]
  000000014094D7CD: movsldup    xmm1,xmm1
  000000014094D7D1: mulps       xmm1,xmm2
  000000014094D7D4: movsldup    xmm0,xmm0
  000000014094D7D8: mulps       xmm0,xmm3
  000000014094D7DB: addps       xmm0,xmm1
  000000014094D7DE: movlps      qword ptr [rsp+50h],xmm0
  000000014094D7E3: lea         rcx,[rsp+70h]
  000000014094D7E8: lea         rdx,[rsp+50h]
  000000014094D7ED: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094D7F2: movaps      xmm0,xmmword ptr [__xmm@00000000000000003f2e147b3f2e147b]
  000000014094D7F9: mulps       xmm0,xmm12
  000000014094D7FD: movss       xmm1,dword ptr [__real@3f2e147b]
  000000014094D805: mulss       xmm1,xmm10
  000000014094D80A: mulss       xmm1,dword ptr [__real@3f000000]
  000000014094D812: mulps       xmm0,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094D819: movsd       xmm2,mmword ptr [rsp+70h]
  000000014094D81F: addps       xmm2,xmm0
  000000014094D822: movaps      xmmword ptr [rsp+0D0h],xmm2
  000000014094D82A: addss       xmm1,dword ptr [rsp+78h]
  000000014094D830: movaps      xmmword ptr [rsp+0E0h],xmm1
  000000014094D838: mov         rsi,qword ptr [rsp+0A8h]
  000000014094D840: mov         rax,qword ptr [rsi]
  000000014094D843: mov         rcx,qword ptr [rsi+8]
  000000014094D847: add         rcx,rax
  000000014094D84A: inc         rcx
  000000014094D84D: movdqa      xmm0,xmmword ptr [rax]
  000000014094D851: pmovmskb    edx,xmm0
  000000014094D855: not         edx
  000000014094D857: mov         word ptr [rsp+88h],dx
  000000014094D85F: mov         qword ptr [rsp+70h],rax
  000000014094D864: add         rax,10h
  000000014094D868: mov         qword ptr [rsp+78h],rax
  000000014094D86D: mov         qword ptr [rsp+80h],rcx
  000000014094D875: mov         rax,qword ptr [rsi+18h]
  000000014094D879: mov         qword ptr [rsp+90h],rax
  000000014094D881: lea         r8,[anon.65ceffb69360efd323e48f4f8756afaa.5.llvm.13316938065337964131]
  000000014094D888: lea         rcx,[rsp+50h]
  000000014094D88D: lea         rdx,[rsp+70h]
  000000014094D892: call        _ZN98_$LT$alloc..vec..Vec$LT$T$GT$$u20$as$u20$alloc..vec..spec_from_iter..SpecFromIter$LT$T$C$I$GT$$GT$9from_iter17h00e18452760e19d7E
  000000014094D897: mov         rcx,qword ptr [rsp+58h]
  000000014094D89C: mov         rdx,qword ptr [rsp+60h]
  000000014094D8A1: test        rdx,rdx
  000000014094D8A4: mov         rax,qword ptr [rsp+68h]
  000000014094D8A9: je          000000014094D9DE
  000000014094D8AF: shl         rdx,3
  000000014094D8B3: xor         r8d,r8d
  000000014094D8B6: pxor        xmm0,xmm0
  000000014094D8BA: cmp         r12d,3
  000000014094D8BE: jae         000000014094D93E
  000000014094D8C0: movss       xmm1,dword ptr [__real@3e800000]
  000000014094D8C8: jmp         000000014094D8DD
  000000014094D8CA: nop         word ptr [rax+rax]
  000000014094D8D0: add         r8,8
  000000014094D8D4: cmp         rdx,r8
  000000014094D8D7: je          000000014094D9DE
  000000014094D8DD: mov         r9,qword ptr [rcx+r8]
  000000014094D8E1: movsd       xmm2,mmword ptr [r9+14h]
  000000014094D8E7: mulps       xmm2,xmm12
  000000014094D8EB: movshdup    xmm3,xmm2
  000000014094D8EF: addss       xmm3,xmm2
  000000014094D8F3: movss       xmm2,dword ptr [r9+1Ch]
  000000014094D8F9: mulss       xmm2,xmm10
  000000014094D8FE: addss       xmm2,xmm3
  000000014094D902: ucomiss     xmm0,xmm2
  000000014094D905: ja          000000014094D8D0
  000000014094D907: movss       xmm2,dword ptr [r9+0Ch]
  000000014094D90D: subss       xmm2,xmm8
  000000014094D912: mulss       xmm2,xmm2
  000000014094D916: movss       xmm3,dword ptr [r9+10h]
  000000014094D91C: movss       xmm4,dword ptr [r9+8]
  000000014094D922: unpcklps    xmm4,xmm3
  000000014094D925: subps       xmm4,xmm7
  000000014094D928: mulps       xmm4,xmm4
  000000014094D92B: addss       xmm2,xmm4
  000000014094D92F: movshdup    xmm3,xmm4
  000000014094D933: addss       xmm3,xmm2
  000000014094D937: ucomiss     xmm1,xmm3
  000000014094D93A: jbe         000000014094D8D0
  000000014094D93C: jmp         000000014094D9BC
  000000014094D93E: movss       xmm1,dword ptr [__real@4023d70b]
  000000014094D946: jmp         000000014094D95D
  000000014094D948: nop         dword ptr [rax+rax]
  000000014094D950: add         r8,8
  000000014094D954: cmp         rdx,r8
  000000014094D957: je          000000014094D9DE
  000000014094D95D: mov         r9,qword ptr [rcx+r8]
  000000014094D961: movsd       xmm2,mmword ptr [r9+14h]
  000000014094D967: mulps       xmm2,xmm12
  000000014094D96B: movshdup    xmm3,xmm2
  000000014094D96F: addss       xmm3,xmm2
  000000014094D973: movss       xmm2,dword ptr [r9+1Ch]
  000000014094D979: mulss       xmm2,xmm10
  000000014094D97E: addss       xmm2,xmm3
  000000014094D982: ucomiss     xmm0,xmm2
  000000014094D985: ja          000000014094D950
  000000014094D987: movss       xmm2,dword ptr [r9+0Ch]
  000000014094D98D: subss       xmm2,xmm8
  000000014094D992: mulss       xmm2,xmm2
  000000014094D996: movss       xmm3,dword ptr [r9+10h]
  000000014094D99C: movss       xmm4,dword ptr [r9+8]
  000000014094D9A2: unpcklps    xmm4,xmm3
  000000014094D9A5: subps       xmm4,xmm7
  000000014094D9A8: mulps       xmm4,xmm4
  000000014094D9AB: addss       xmm2,xmm4
  000000014094D9AF: movshdup    xmm3,xmm4
  000000014094D9B3: addss       xmm3,xmm2
  000000014094D9B7: ucomiss     xmm1,xmm3
  000000014094D9BA: jbe         000000014094D950
  000000014094D9BC: mov         rdx,qword ptr [rsp+50h]
  000000014094D9C1: test        rdx,rdx
  000000014094D9C4: je          000000014094DE8C
  000000014094D9CA: shl         rdx,3
  000000014094D9CE: mov         r8d,8
  000000014094D9D4: call        __rust_dealloc
  000000014094D9D9: jmp         000000014094DE8C
  000000014094D9DE: movaps      xmmword ptr [rsp+110h],xmm12
  000000014094D9E7: movaps      xmmword ptr [rsp+120h],xmm10
  000000014094D9F0: mov         rdx,qword ptr [rsp+50h]
  000000014094D9F5: test        rdx,rdx
  000000014094D9F8: je          000000014094DA0E
  000000014094D9FA: shl         rdx,3
  000000014094D9FE: mov         r8d,8
  000000014094DA04: call        __rust_dealloc
  000000014094DA09: mov         rax,qword ptr [rsp+68h]
  000000014094DA0E: mov         ecx,dword ptr [rax+1Ch]
  000000014094DA11: mov         rax,qword ptr [rax+10h]
  000000014094DA15: mov         dword ptr [rsp+48h],ecx
  000000014094DA19: mov         qword ptr [rsp+0A0h],rax
  000000014094DA21: mov         dword ptr [rax],ecx
  000000014094DA23: lea         rcx,[rsi+30h]
  000000014094DA27: mov         edx,3
  000000014094DA2C: mov         qword ptr [rsp+108h],rcx
  000000014094DA34: call        _ZN9perchance16PerchanceContext15usize_less_than17hdfd47e9d39dde918E
  000000014094DA39: add         rax,2
  000000014094DA3D: mov         qword ptr [rsp+68h],rax
  000000014094DA42: je          000000014094DE8C
  000000014094DA48: xor         eax,eax
  000000014094DA4A: cmp         r12d,3
  000000014094DA4E: setb        al
  000000014094DA51: mov         rcx,qword ptr [r15]
  000000014094DA54: mov         qword ptr [rsp+100h],rcx
  000000014094DA5C: lea         rcx,[__real@3f80000040400000]
  000000014094DA63: movss       xmm0,dword ptr [rcx+rax*4]
  000000014094DA68: movss       dword ptr [rsp+0BCh],xmm0
  000000014094DA71: movaps      xmm15,xmmword ptr [rsp+0E0h]
  000000014094DA7A: movaps      xmm0,xmmword ptr [rsp+0D0h]
  000000014094DA82: movlhps     xmm15,xmm0
  000000014094DA86: shufps      xmm15,xmm0,0E2h
  000000014094DA8B: lea         r14,[rsp+50h]
  000000014094DA90: lea         r15,[rsp+70h]
  000000014094DA95: lea         r12,[rsp+0C4h]
  000000014094DA9D: movaps      xmm12,xmmword ptr [__xmm@0000000000000000bf000000bf000000]
  000000014094DAA5: movss       xmm6,dword ptr [__real@bf000000]
  000000014094DAAD: movss       xmm13,dword ptr [__real@3f000000]
  000000014094DAB6: movss       xmm7,dword ptr [__real@bea147ae]
  000000014094DABE: movss       xmm8,dword ptr [__real@3ea147ae]
  000000014094DAC7: xor         edi,edi
  000000014094DAC9: jmp         000000014094DB6E
  000000014094DACE: mov         eax,dword ptr [rsp+48h]
  000000014094DAD2: mov         rcx,qword ptr [rsp+0A0h]
  000000014094DADA: mov         dword ptr [rcx],eax
  000000014094DADC: movaps      xmm0,xmmword ptr [rsp+0D0h]
  000000014094DAE4: movlps      qword ptr [rsp+50h],xmm0
  000000014094DAE9: movaps      xmm0,xmmword ptr [rsp+0E0h]
  000000014094DAF1: movss       dword ptr [rsp+58h],xmm0
  000000014094DAF7: movaps      xmm0,xmmword ptr [rsp+110h]
  000000014094DAFF: movlps      qword ptr [rsp+70h],xmm0
  000000014094DB04: movaps      xmm0,xmmword ptr [rsp+120h]
  000000014094DB0C: movss       dword ptr [rsp+78h],xmm0
  000000014094DB12: mov         rax,qword ptr [rsp+0F8h]
  000000014094DB1A: mov         qword ptr [rsp+38h],rax
  000000014094DB1F: mov         qword ptr [rsp+30h],rsi
  000000014094DB24: movss       xmm0,dword ptr [rsp+0BCh]
  000000014094DB2D: movss       dword ptr [rsp+20h],xmm0
  000000014094DB33: mov         qword ptr [rsp+28h],14h
  000000014094DB3C: xorps       xmm3,xmm3
  000000014094DB3F: mov         rcx,qword ptr [rsp+0A8h]
  000000014094DB47: mov         rdx,r14
  000000014094DB4A: mov         r8,r15
  000000014094DB4D: call        _ZN12country_core9resources3ivy11ivy_storage10IvyStorage9spawn_ivy17hf1a9d0b38d1a8dd5E
  000000014094DB52: nop         word ptr cs:[rax+rax]
  000000014094DB60: inc         rdi
  000000014094DB63: cmp         rdi,qword ptr [rsp+68h]
  000000014094DB68: je          000000014094DE8C
  000000014094DB6E: mov         eax,9C41h
  000000014094DB73: sub         rax,rbp
  000000014094DB76: cmp         rax,46h
  000000014094DB7A: mov         esi,46h
  000000014094DB7F: cmovb       rsi,rax
  000000014094DB83: add         rbp,rsi
  000000014094DB86: dec         rbp
  000000014094DB89: cmp         rbp,9C41h
  000000014094DB90: setae       cl
  000000014094DB93: cmp         rax,14h
  000000014094DB97: setb        al
  000000014094DB9A: or          al,cl
  000000014094DB9C: jne         000000014094DB60
  000000014094DB9E: mov         eax,dword ptr [rsp+48h]
  000000014094DBA2: mov         rcx,qword ptr [rsp+0A0h]
  000000014094DBAA: mov         dword ptr [rcx],eax
  000000014094DBAC: mov         rcx,qword ptr [rsp+108h]
  000000014094DBB4: call        _ZN9perchance16PerchanceContext11uniform_f3217h050ccfebc3016512E
  000000014094DBB9: mulss       xmm0,dword ptr [__real@3fc00000]
  000000014094DBC1: addss       xmm0,dword ptr [__real@bf400000]
  000000014094DBC9: divss       xmm0,dword ptr [rsp+0C0h]
  000000014094DBD2: addss       xmm0,dword ptr [rsp+4Ch]
  000000014094DBD8: ucomiss     xmm0,xmm0
  000000014094DBDB: jp          000000014094DEF8
  000000014094DBE1: movd        eax,xmm0
  000000014094DBE5: and         eax,7FFFFFFFh
  000000014094DBEA: cmp         eax,7F800000h
  000000014094DBEF: je          000000014094DF10
  000000014094DBF5: ucomiss     xmm11,xmm0
  000000014094DBF9: jb          000000014094DB60
  000000014094DBFF: ucomiss     xmm0,dword ptr [__real@00000000]
  000000014094DC06: jb          000000014094DB60
  000000014094DC0C: mov         rbx,qword ptr [rsp+100h]
  000000014094DC14: movsd       xmm0,mmword ptr [rbx+50h]
  000000014094DC19: movaps      xmm9,xmm0
  000000014094DC1D: mulps       xmm9,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014094DC25: addps       xmm9,xmm15
  000000014094DC29: movsd       xmm14,mmword ptr [rbx+48h]
  000000014094DC2F: cvtdq2ps    xmm1,xmm14
  000000014094DC33: divps       xmm0,xmm1
  000000014094DC36: divps       xmm9,xmm0
  000000014094DC3A: movshdup    xmm0,xmm9
  000000014094DC3F: call        floorf
  000000014094DC44: movaps      xmm10,xmm0
  000000014094DC48: movaps      xmm0,xmm9
  000000014094DC4C: call        floorf
  000000014094DC51: cvttss2si   eax,xmm0
  000000014094DC55: movss       xmm1,dword ptr [__real@4effffff]
  000000014094DC5D: ucomiss     xmm0,xmm1
  000000014094DC60: mov         r8d,7FFFFFFFh
  000000014094DC66: cmova       eax,r8d
  000000014094DC6A: ucomiss     xmm0,xmm0
  000000014094DC6D: mov         ecx,0
  000000014094DC72: cmovp       eax,ecx
  000000014094DC75: cvttss2si   edx,xmm10
  000000014094DC7A: ucomiss     xmm10,xmm1
  000000014094DC7E: cmova       edx,r8d
  000000014094DC82: ucomiss     xmm10,xmm10
  000000014094DC86: cmovp       edx,ecx
  000000014094DC89: mov         ecx,8
  000000014094DC8E: test        eax,eax
  000000014094DC90: js          000000014094DCF6
  000000014094DC92: movd        xmm0,eax
  000000014094DC96: movd        xmm1,edx
  000000014094DC9A: punpckldq   xmm0,xmm1
  000000014094DC9E: movq        xmm0,xmm0
  000000014094DCA2: movaps      xmm1,xmm14
  000000014094DCA6: pcmpgtd     xmm1,xmm0
  000000014094DCAA: pshufd      xmm1,xmm1,50h
  000000014094DCAF: movmskpd    edx,xmm1
  000000014094DCB3: test        dl,2
  000000014094DCB6: je          000000014094DCF6
  000000014094DCB8: test        dl,1
  000000014094DCBB: je          000000014094DCF6
  000000014094DCBD: pshufd      xmm0,xmm0,55h
  000000014094DCC2: movd        edx,xmm0
  000000014094DCC6: test        edx,edx
  000000014094DCC8: js          000000014094DCF6
  000000014094DCCA: movd        r8d,xmm14
  000000014094DCCF: imul        edx,r8d
  000000014094DCD3: add         edx,eax
  000000014094DCD5: movsxd      rax,edx
  000000014094DCD8: cmp         qword ptr [rbx+10h],rax
  000000014094DCDC: jbe         000000014094DCF6
  000000014094DCDE: mov         rdx,qword ptr [rbx+8]
  000000014094DCE2: lea         rax,[rax+rax*2]
  000000014094DCE6: shl         rax,4
  000000014094DCEA: mov         rcx,qword ptr [rdx+rax+8]
  000000014094DCEF: mov         rax,qword ptr [rdx+rax+10h]
  000000014094DCF4: jmp         000000014094DCF8
  000000014094DCF6: xor         eax,eax
  000000014094DCF8: imul        rbx,rax,38h
  000000014094DCFC: nop         dword ptr [rax]
  000000014094DD00: test        rbx,rbx
  000000014094DD03: je          000000014094DACE
  000000014094DD09: lea         r13,[rcx+38h]
  000000014094DD0D: movsd       xmm14,mmword ptr [rcx]
  000000014094DD12: movsd       xmm10,mmword ptr [rcx+8]
  000000014094DD18: movaps      xmm0,xmm10
  000000014094DD1C: subps       xmm0,xmm14
  000000014094DD20: movaps      xmm1,xmm0
  000000014094DD23: mulps       xmm1,xmm0
  000000014094DD26: movshdup    xmm2,xmm1
  000000014094DD2A: addss       xmm2,xmm1
  000000014094DD2E: xorps       xmm1,xmm1
  000000014094DD31: sqrtss      xmm1,xmm2
  000000014094DD35: movaps      xmm2,xmm11
  000000014094DD39: divss       xmm2,xmm1
  000000014094DD3D: movsldup    xmm1,xmm2
  000000014094DD41: mulps       xmm1,xmm0
  000000014094DD44: movlps      qword ptr [rsp+0B0h],xmm1
  000000014094DD4C: mov         rcx,r12
  000000014094DD4F: lea         rdx,[rsp+0B0h]
  000000014094DD57: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094DD5C: mov         dword ptr [rsp+58h],3F800000h
  000000014094DD64: mov         qword ptr [rsp+50h],0
  000000014094DD6D: mov         rcx,r15
  000000014094DD70: mov         rdx,r12
  000000014094DD73: mov         r8,r14
  000000014094DD76: call        _ZN4glam3f324sse24quat4Quat17from_rotation_arc17h31be4e9a5a179391E
  000000014094DD7B: movaps      xmm9,xmmword ptr [rsp+70h]
  000000014094DD81: addps       xmm10,xmm14
  000000014094DD85: mulps       xmm10,xmm12
  000000014094DD89: addps       xmm10,xmm15
  000000014094DD8D: movlps      qword ptr [rsp+50h],xmm10
  000000014094DD93: mov         rcx,r15
  000000014094DD96: mov         rdx,r14
  000000014094DD99: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerX0Y$GT$3x0y17h6dae267728567868E
  000000014094DD9E: movsd       xmm0,mmword ptr [rsp+70h]
  000000014094DDA4: movss       xmm2,dword ptr [rsp+78h]
  000000014094DDAA: shufps      xmm2,xmm0,30h
  000000014094DDAE: movaps      xmm1,xmm0
  000000014094DDB1: shufps      xmm1,xmm2,84h
  000000014094DDB5: movaps      xmm3,xmm9
  000000014094DDB9: mulps       xmm3,xmm9
  000000014094DDBD: movshdup    xmm2,xmm3
  000000014094DDC1: addss       xmm2,xmm3
  000000014094DDC5: movaps      xmm4,xmm3
  000000014094DDC8: unpckhpd    xmm4,xmm3
  000000014094DDCC: addss       xmm4,xmm2
  000000014094DDD0: shufps      xmm4,xmm4,0
  000000014094DDD4: shufps      xmm3,xmm3,0FFh
  000000014094DDD8: subps       xmm3,xmm4
  000000014094DDDB: mulps       xmm3,xmm1
  000000014094DDDE: movaps      xmm2,xmm9
  000000014094DDE2: mulps       xmm2,xmm1
  000000014094DDE5: movshdup    xmm4,xmm2
  000000014094DDE9: addss       xmm4,xmm2
  000000014094DDED: movhlps     xmm2,xmm2
  000000014094DDF0: addss       xmm2,xmm4
  000000014094DDF4: addss       xmm2,xmm2
  000000014094DDF8: shufps      xmm2,xmm2,0
  000000014094DDFC: mulps       xmm2,xmm9
  000000014094DE00: addps       xmm2,xmm3
  000000014094DE03: movaps      xmm3,xmm9
  000000014094DE07: shufps      xmm3,xmm9,0D0h
  000000014094DE0C: shufps      xmm0,xmm0,0D0h
  000000014094DE10: mulps       xmm3,xmm1
  000000014094DE13: mulps       xmm0,xmm9
  000000014094DE17: subps       xmm3,xmm0
  000000014094DE1A: shufps      xmm3,xmm3,0D6h
  000000014094DE1E: addps       xmm9,xmm9
  000000014094DE22: shufps      xmm9,xmm9,0FFh
  000000014094DE27: mulps       xmm9,xmm3
  000000014094DE2B: addps       xmm9,xmm2
  000000014094DE2F: movaps      xmm0,xmm9
  000000014094DE33: unpckhpd    xmm0,xmm9
  000000014094DE38: movsd       xmm1,mmword ptr [rsp+0B0h]
  000000014094DE41: mulps       xmm1,xmm1
  000000014094DE44: movshdup    xmm2,xmm1
  000000014094DE48: addss       xmm2,xmm1
  000000014094DE4C: xorps       xmm3,xmm3
  000000014094DE4F: sqrtss      xmm3,xmm2
  000000014094DE53: movaps      xmm1,xmm3
  000000014094DE56: mulss       xmm1,xmm6
  000000014094DE5A: mulss       xmm3,xmm13
  000000014094DE5F: movss       dword ptr [rsp+20h],xmm9
  000000014094DE66: movss       dword ptr [rsp+28h],xmm0
  000000014094DE6C: movaps      xmm0,xmm7
  000000014094DE6F: movaps      xmm2,xmm8
  000000014094DE73: call        _ZN5utils8geometry4aabb13is_inside_bbx17h1645272c51b82155E
  000000014094DE78: add         rbx,0FFFFFFFFFFFFFFC8h
  000000014094DE7C: mov         rcx,r13
  000000014094DE7F: test        al,al
  000000014094DE81: je          000000014094DD00
  000000014094DE87: jmp         000000014094DB60
  000000014094DE8C: movaps      xmm6,xmmword ptr [rsp+130h]
  000000014094DE94: movaps      xmm7,xmmword ptr [rsp+140h]
  000000014094DE9C: movaps      xmm8,xmmword ptr [rsp+150h]
  000000014094DEA5: movaps      xmm9,xmmword ptr [rsp+160h]
  000000014094DEAE: movaps      xmm10,xmmword ptr [rsp+170h]
  000000014094DEB7: movaps      xmm11,xmmword ptr [rsp+180h]
  000000014094DEC0: movaps      xmm12,xmmword ptr [rsp+190h]
  000000014094DEC9: movaps      xmm13,xmmword ptr [rsp+1A0h]
  000000014094DED2: movaps      xmm14,xmmword ptr [rsp+1B0h]
  000000014094DEDB: movaps      xmm15,xmmword ptr [rsp+1C0h]
  000000014094DEE4: add         rsp,1D8h
  000000014094DEEB: pop         rbx
  000000014094DEEC: pop         rbp
  000000014094DEED: pop         rdi
  000000014094DEEE: pop         rsi
  000000014094DEEF: pop         r12
  000000014094DEF1: pop         r13
  000000014094DEF3: pop         r14
  000000014094DEF5: pop         r15
  000000014094DEF7: ret
  000000014094DEF8: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.14.llvm.3277990281340110158]
  000000014094DEFF: lea         r8,[142AB3878h]
  000000014094DF06: mov         edx,1Dh
  000000014094DF0B: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094DF10: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.19.llvm.3277990281340110158]
  000000014094DF17: lea         r8,[142AB3878h]
  000000014094DF1E: mov         edx,22h
  000000014094DF23: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094DF28: lea         rax,[rsp+0B0h]
  000000014094DF30: mov         qword ptr [rsp+50h],rax
  000000014094DF35: lea         rax,[_ZN4core3fmt5float52_$LT$impl$u20$core..fmt..Display$u20$for$u20$f32$GT$3fmt17h06731e4b2f3e60e1E]
  000000014094DF3C: mov         qword ptr [rsp+58h],rax
  000000014094DF41: lea         rax,[anon.12ce4fde5425384d5f438f5122902e1a.13.llvm.3277990281340110158]
  000000014094DF48: mov         qword ptr [rsp+70h],rax
  000000014094DF4D: mov         qword ptr [rsp+78h],1
  000000014094DF56: mov         qword ptr [rsp+90h],0
  000000014094DF62: lea         rax,[rsp+50h]
  000000014094DF67: mov         qword ptr [rsp+80h],rax
  000000014094DF6F: mov         qword ptr [rsp+88h],1
  000000014094DF7B: lea         rdx,[142AB38A8h]
  000000014094DF82: lea         rcx,[rsp+70h]
  000000014094DF87: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  000000014094DF8C: lea         rcx,[anon.12ce4fde5425384d5f438f5122902e1a.14.llvm.3277990281340110158]
  000000014094DF93: lea         r8,[142AB38A8h]
  000000014094DF9A: mov         edx,1Dh
  000000014094DF9F: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094DFA4: lea         rcx,[anon.8b73eb39c06afd88b250ecc24c523fbb.44.llvm.11914376241322277501]
  000000014094DFAB: lea         r8,[142AB3830h]
  000000014094DFB2: mov         edx,32h
  000000014094DFB7: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014094DFBC: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.42.llvm.12803187488448158381]
  000000014094DFC3: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094DFC8: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.43.llvm.12803187488448158381]
  000000014094DFCF: mov         rcx,rax
  000000014094DFD2: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094DFD7: lea         r8,[anon.0d0a89084ed777ac73f29d93318bb777.44.llvm.12803187488448158381]
  000000014094DFDE: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094DFE3: lea         r8,[142AB38C0h]
  000000014094DFEA: mov         rdx,rbx
  000000014094DFED: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014094DFF2: int         3
  000000014094DFF3: CC CC CC CC CC CC CC CC CC CC CC CC CC           .............

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

; ===== _ZN12country_core7systems3ivy10ivy_pruner22remove_ivy_at_location17h81e469fca5672e93E:
  0000000140999860: push        r15
  0000000140999862: push        r14
  0000000140999864: push        r12
  0000000140999866: push        rsi
  0000000140999867: push        rdi
  0000000140999868: push        rbx
  0000000140999869: sub         rsp,48h
  000000014099986D: movaps      xmmword ptr [rsp+30h],xmm7
  0000000140999872: movaps      xmmword ptr [rsp+20h],xmm6
  0000000140999877: mov         rdi,r9
  000000014099987A: movaps      xmm7,xmm2
  000000014099987D: movaps      xmm6,xmm1
  0000000140999880: mov         rsi,rcx
  0000000140999883: mov         r12,qword ptr [rsp+0A0h]
  000000014099988B: mov         rax,qword ptr [rsp+0A8h]
  0000000140999893: mov         ecx,dword ptr [rax+1Ch]
  0000000140999896: mov         rdx,qword ptr [rax+10h]
  000000014099989A: mov         dword ptr [rdx],ecx
  000000014099989C: mov         rbx,qword ptr [rax]
  000000014099989F: mov         r14,qword ptr [rbx+30h]
  00000001409998A3: mov         r15,qword ptr [rbx+40h]
  00000001409998A7: cmp         r14,qword ptr [rbx+20h]
  00000001409998AB: jne         00000001409998BD
  00000001409998AD: lea         rcx,[rbx+20h]
  00000001409998B1: lea         rdx,[anon.60360aed504c4a0f85b3886feb406ce0.1.llvm.17420070914341975340]
  00000001409998B8: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h10fe452488757892E
  00000001409998BD: mov         rax,qword ptr [rbx+28h]
  00000001409998C1: lea         rcx,[r14+r14*2]
  00000001409998C5: shl         rcx,4
  00000001409998C9: mov         qword ptr [rax+rcx],rdi
  00000001409998CD: mov         qword ptr [rax+rcx+8],r12
  00000001409998D2: mov         rdx,qword ptr [rsi]
  00000001409998D5: mov         qword ptr [rax+rcx+10h],rdx
  00000001409998DA: mov         edx,dword ptr [rsi+8]
  00000001409998DD: mov         dword ptr [rax+rcx+18h],edx
  00000001409998E1: movss       dword ptr [rax+rcx+1Ch],xmm6
  00000001409998E7: movss       dword ptr [rax+rcx+20h],xmm7
  00000001409998ED: movss       dword ptr [rax+rcx+24h],xmm6
  00000001409998F3: mov         qword ptr [rax+rcx+28h],r15
  00000001409998F8: inc         r14
  00000001409998FB: mov         qword ptr [rbx+30h],r14
  00000001409998FF: inc         qword ptr [rbx+40h]
  0000000140999903: movaps      xmm6,xmmword ptr [rsp+20h]
  0000000140999908: movaps      xmm7,xmmword ptr [rsp+30h]
  000000014099990D: add         rsp,48h
  0000000140999911: pop         rbx
  0000000140999912: pop         rdi
  0000000140999913: pop         rsi
  0000000140999914: pop         r12
  0000000140999916: pop         r14
  0000000140999918: pop         r15
  000000014099991A: ret
  000000014099991B: CC CC CC CC CC                                   .....

; ===== _ZN12country_core7startup11startup_ivy11startup_ivy17h9cd03b74ef24a4ccE:
  0000000140A21A30: push        rbp
  0000000140A21A31: push        r15
  0000000140A21A33: push        r14
  0000000140A21A35: push        r13
  0000000140A21A37: push        r12
  0000000140A21A39: push        rsi
  0000000140A21A3A: push        rdi
  0000000140A21A3B: push        rbx
  0000000140A21A3C: sub         rsp,178h
  0000000140A21A43: lea         rbp,[rsp+80h]
  0000000140A21A4B: movaps      xmmword ptr [rbp+0E0h],xmm6
  0000000140A21A52: mov         qword ptr [rbp+0D8h],0FFFFFFFFFFFFFFFEh
  0000000140A21A5D: mov         rbx,r9
  0000000140A21A60: mov         rsi,r8
  0000000140A21A63: mov         rdi,rdx
  0000000140A21A66: mov         r14,rcx
  0000000140A21A69: movaps      xmm0,xmmword ptr [__xmm@3fc00000bfc000003dcccccdbdcccccd]
  0000000140A21A70: movaps      xmmword ptr [rbp-30h],xmm0
  0000000140A21A74: movsd       xmm0,mmword ptr [__xmm@00000000000000003dcccccdbdcccccd]
  0000000140A21A7C: movsd       mmword ptr [rbp-20h],xmm0
  0000000140A21A81: lea         r15,[rbp+60h]
  0000000140A21A85: lea         rdx,[rbp-30h]
  0000000140A21A89: mov         rcx,r15
  0000000140A21A8C: call        _ZN12country_core8geometry4cube123_$LT$impl$u20$core..convert..From$LT$country_core..geometry..cube..Box$GT$$u20$for$u20$country_core..render..mesh..Mesh$GT$4from17h88dd8088f2afdd3dE
  0000000140A21A91: movss       xmm0,dword ptr [__real@3f800000]
  0000000140A21A99: movlps      qword ptr [rbp-40h],xmm0
  0000000140A21A9D: mov         dword ptr [rbp-38h],0
  0000000140A21AA4: lea         r12,[rbp-30h]
  0000000140A21AA8: lea         r8,[rbp-40h]
  0000000140A21AAC: mov         rcx,r12
  0000000140A21AAF: mov         rdx,r15
  0000000140A21AB2: call        _ZN12country_core6render4mesh4Mesh14add_color_self17h7620fb3fbf0eae5eE
  0000000140A21AB7: mov         rsi,qword ptr [rsi]
  0000000140A21ABA: mov         r13,qword ptr [rdi]
  0000000140A21ABD: mov         qword ptr [rsp+20h],r13
  0000000140A21AC2: lea         rdx,[142AC9723h]
  0000000140A21AC9: mov         r8d,0Eh
  0000000140A21ACF: mov         rcx,r12
  0000000140A21AD2: mov         r9,rsi
  0000000140A21AD5: call        _ZN12country_core7startup22load_mesh_into_library17h9943ce76e264ec52E
  0000000140A21ADA: lea         rdx,[142AC9731h]
  0000000140A21AE1: lea         rcx,[rbp+60h]
  0000000140A21AE5: mov         r8d,1Dh
  0000000140A21AEB: call        _ZN12country_core5utils9load_json17load_json_as_mesh17h16c972904558beceE
  0000000140A21AF0: xor         eax,eax
  0000000140A21AF2: cmp         rax,qword ptr [rbp+60h]
  0000000140A21AF6: jo          0000000140A21FE4
  0000000140A21AFC: mov         rdi,qword ptr [rbp+168h]
  0000000140A21B03: mov         r12,qword ptr [rbp+160h]
  0000000140A21B0A: mov         rax,qword ptr [rbp+0D0h]
  0000000140A21B11: mov         qword ptr [rbp+40h],rax
  0000000140A21B15: movups      xmm0,xmmword ptr [rbp+0C0h]
  0000000140A21B1C: movaps      xmmword ptr [rbp+30h],xmm0
  0000000140A21B20: movups      xmm0,xmmword ptr [rbp+0B0h]
  0000000140A21B27: movaps      xmmword ptr [rbp+20h],xmm0
  0000000140A21B2B: movups      xmm0,xmmword ptr [rbp+0A0h]
  0000000140A21B32: movaps      xmmword ptr [rbp+10h],xmm0
  0000000140A21B36: movups      xmm0,xmmword ptr [rbp+60h]
  0000000140A21B3A: movups      xmm1,xmmword ptr [rbp+70h]
  0000000140A21B3E: movups      xmm2,xmmword ptr [rbp+80h]
  0000000140A21B45: movups      xmm3,xmmword ptr [rbp+90h]
  0000000140A21B4C: movaps      xmmword ptr [rbp],xmm3
  0000000140A21B50: movaps      xmmword ptr [rbp-10h],xmm2
  0000000140A21B54: movaps      xmmword ptr [rbp-20h],xmm1
  0000000140A21B58: movaps      xmmword ptr [rbp-30h],xmm0
  0000000140A21B5C: mov         qword ptr [rsp+20h],r13
  0000000140A21B61: lea         rdx,[142AC9798h]
  0000000140A21B68: lea         rcx,[rbp-30h]
  0000000140A21B6C: mov         r8d,0Ah
  0000000140A21B72: mov         r9,rsi
  0000000140A21B75: call        _ZN12country_core7startup22load_mesh_into_library17h9943ce76e264ec52E
  0000000140A21B7A: mov         r15,rax
  0000000140A21B7D: mov         rax,qword ptr [r14]
  0000000140A21B80: mov         qword ptr [rbp+50h],rax
  0000000140A21B84: mov         rcx,qword ptr [rax]
  0000000140A21B87: add         rcx,10h
  0000000140A21B8B: mov         rax,qword ptr [r12]
  0000000140A21B8F: mov         rdx,qword ptr [rbx]
  0000000140A21B92: mov         qword ptr [rbp+58h],rdx
  0000000140A21B96: mov         qword ptr [rsp+30h],rdx
  0000000140A21B9B: mov         qword ptr [rbp+48h],rax
  0000000140A21B9F: mov         qword ptr [rsp+28h],rax
  0000000140A21BA4: mov         qword ptr [rsp+20h],1Bh
  0000000140A21BAD: lea         rdx,[142AC97A2h]
  0000000140A21BB4: lea         r9,[142AC97C7h]
  0000000140A21BBB: mov         r8d,25h
  0000000140A21BC1: call        _ZN12country_core7startup24load_shader_into_library17haa83c8b937a2393cE
  0000000140A21BC6: mov         rbx,rax
  0000000140A21BC9: lea         rcx,[rbp+60h]
  0000000140A21BCD: mov         rdx,rdi
  0000000140A21BD0: call        _ZN8bevy_ecs6system8commands8Commands11spawn_empty17hdff45581613ea8c5E
  0000000140A21BD5: mov         r14,qword ptr [rbp+68h]
  0000000140A21BD9: test        r14,r14
  0000000140A21BDC: jne         0000000140A21BE2
  0000000140A21BDE: mov         r14,qword ptr [rbp+70h]
  0000000140A21BE2: mov         rdi,qword ptr [rbp+80h]
  0000000140A21BE9: mov         rax,qword ptr [r14]
  0000000140A21BEC: mov         r12,qword ptr [r14+10h]
  0000000140A21BF0: sub         rax,r12
  0000000140A21BF3: cmp         rax,77h
  0000000140A21BF7: jbe         0000000140A21F75
  0000000140A21BFD: mov         rax,qword ptr [r14+8]
  0000000140A21C01: lea         rcx,[_ZN4core3ops8function6FnOnce9call_once17hf29b79776c9d74aaE.llvm.11137708931887948641]
  0000000140A21C08: mov         qword ptr [rax+r12],rcx
  0000000140A21C0C: movaps      xmm6,xmmword ptr [__xmm@3f800000000000000000000000000000]
  0000000140A21C13: movups      xmmword ptr [rax+r12+8],xmm6
  0000000140A21C19: mov         qword ptr [rax+r12+18h],0
  0000000140A21C22: mov         dword ptr [rax+r12+20h],0
  0000000140A21C2B: mov         rcx,3F8000003F800000h
  0000000140A21C35: mov         qword ptr [rax+r12+24h],rcx
  0000000140A21C3A: mov         dword ptr [rax+r12+2Ch],3F800000h
  0000000140A21C43: mov         qword ptr [rax+r12+38h],r15
  0000000140A21C48: mov         qword ptr [rax+r12+40h],rbx
  0000000140A21C4D: mov         byte ptr [rax+r12+48h],0
  0000000140A21C53: mov         qword ptr [rax+r12+58h],rdi
  0000000140A21C58: lea         rcx,[_ZN8bevy_ecs5error7handler35panic$u7b$$u7b$reify.shim$u7d$$u7d$17hfabb946af0d30473E.llvm.15983634890098048951]
  0000000140A21C5F: mov         qword ptr [rax+r12+68h],rcx
  0000000140A21C64: add         r12,78h
  0000000140A21C68: mov         qword ptr [r14+10h],r12
  0000000140A21C6C: lea         rdx,[142AC97E2h]
  0000000140A21C73: lea         rcx,[rbp+60h]
  0000000140A21C77: mov         r8d,1Bh
  0000000140A21C7D: call        _ZN12country_core5utils9load_json17load_json_as_mesh17h16c972904558beceE
  0000000140A21C82: xor         eax,eax
  0000000140A21C84: cmp         rax,qword ptr [rbp+60h]
  0000000140A21C88: jo          0000000140A22016
  0000000140A21C8E: mov         rax,qword ptr [rbp+0D0h]
  0000000140A21C95: mov         qword ptr [rbp+40h],rax
  0000000140A21C99: movups      xmm0,xmmword ptr [rbp+0C0h]
  0000000140A21CA0: movaps      xmmword ptr [rbp+30h],xmm0
  0000000140A21CA4: movups      xmm0,xmmword ptr [rbp+0B0h]
  0000000140A21CAB: movaps      xmmword ptr [rbp+20h],xmm0
  0000000140A21CAF: movups      xmm0,xmmword ptr [rbp+0A0h]
  0000000140A21CB6: movaps      xmmword ptr [rbp+10h],xmm0
  0000000140A21CBA: movups      xmm0,xmmword ptr [rbp+60h]
  0000000140A21CBE: movups      xmm1,xmmword ptr [rbp+70h]
  0000000140A21CC2: movups      xmm2,xmmword ptr [rbp+80h]
  0000000140A21CC9: movups      xmm3,xmmword ptr [rbp+90h]
  0000000140A21CD0: movaps      xmmword ptr [rbp],xmm3
  0000000140A21CD4: movaps      xmmword ptr [rbp-10h],xmm2
  0000000140A21CD8: movaps      xmmword ptr [rbp-20h],xmm1
  0000000140A21CDC: movaps      xmmword ptr [rbp-30h],xmm0
  0000000140A21CE0: mov         qword ptr [rsp+20h],r13
  0000000140A21CE5: lea         rdx,[142AC9818h]
  0000000140A21CEC: lea         rcx,[rbp-30h]
  0000000140A21CF0: mov         r8d,8
  0000000140A21CF6: mov         r9,rsi
  0000000140A21CF9: call        _ZN12country_core7startup22load_mesh_into_library17h9943ce76e264ec52E
  0000000140A21CFE: mov         rbx,rax
  0000000140A21D01: mov         rax,qword ptr [rbp+50h]
  0000000140A21D05: mov         rcx,qword ptr [rax]
  0000000140A21D08: add         rcx,10h
  0000000140A21D0C: mov         rax,qword ptr [rbp+58h]
  0000000140A21D10: mov         qword ptr [rsp+30h],rax
  0000000140A21D15: mov         rax,qword ptr [rbp+48h]
  0000000140A21D19: mov         qword ptr [rsp+28h],rax
  0000000140A21D1E: mov         qword ptr [rsp+20h],19h
  0000000140A21D27: lea         rdx,[142AC9820h]
  0000000140A21D2E: lea         r9,[142AC9843h]
  0000000140A21D35: mov         r8d,23h
  0000000140A21D3B: call        _ZN12country_core7startup24load_shader_into_library17haa83c8b937a2393cE
  0000000140A21D40: mov         r14,rax
  0000000140A21D43: lea         rcx,[rbp+60h]
  0000000140A21D47: mov         rdx,qword ptr [rbp+168h]
  0000000140A21D4E: call        _ZN8bevy_ecs6system8commands8Commands11spawn_empty17hdff45581613ea8c5E
  0000000140A21D53: mov         r15,qword ptr [rbp+68h]
  0000000140A21D57: test        r15,r15
  0000000140A21D5A: jne         0000000140A21D60
  0000000140A21D5C: mov         r15,qword ptr [rbp+70h]
  0000000140A21D60: mov         rdi,qword ptr [rbp+80h]
  0000000140A21D67: mov         rax,qword ptr [r15]
  0000000140A21D6A: mov         r12,qword ptr [r15+10h]
  0000000140A21D6E: sub         rax,r12
  0000000140A21D71: cmp         rax,77h
  0000000140A21D75: jbe         0000000140A21F9A
  0000000140A21D7B: mov         rax,qword ptr [r15+8]
  0000000140A21D7F: lea         rcx,[_ZN4core3ops8function6FnOnce9call_once17hf29b79776c9d74aaE.llvm.11137708931887948641]
  0000000140A21D86: mov         qword ptr [rax+r12],rcx
  0000000140A21D8A: movups      xmmword ptr [rax+r12+8],xmm6
  0000000140A21D90: mov         qword ptr [rax+r12+18h],0
  0000000140A21D99: mov         dword ptr [rax+r12+20h],0
  0000000140A21DA2: mov         rcx,3F8000003F800000h
  0000000140A21DAC: mov         qword ptr [rax+r12+24h],rcx
  0000000140A21DB1: mov         dword ptr [rax+r12+2Ch],3F800000h
  0000000140A21DBA: mov         qword ptr [rax+r12+38h],rbx
  0000000140A21DBF: mov         qword ptr [rax+r12+40h],r14
  0000000140A21DC4: mov         byte ptr [rax+r12+48h],0
  0000000140A21DCA: mov         qword ptr [rax+r12+58h],rdi
  0000000140A21DCF: lea         rcx,[_ZN8bevy_ecs5error7handler35panic$u7b$$u7b$reify.shim$u7d$$u7d$17hfabb946af0d30473E.llvm.15983634890098048951]
  0000000140A21DD6: mov         qword ptr [rax+r12+68h],rcx
  0000000140A21DDB: add         r12,78h
  0000000140A21DDF: mov         qword ptr [r15+10h],r12
  0000000140A21DE3: lea         rdx,[142AC985Ch]
  0000000140A21DEA: lea         rcx,[rbp+60h]
  0000000140A21DEE: mov         r8d,1Dh
  0000000140A21DF4: call        _ZN12country_core5utils9load_json17load_json_as_mesh17h16c972904558beceE
  0000000140A21DF9: xor         eax,eax
  0000000140A21DFB: cmp         rax,qword ptr [rbp+60h]
  0000000140A21DFF: jo          0000000140A22048
  0000000140A21E05: mov         rax,qword ptr [rbp+0D0h]
  0000000140A21E0C: mov         qword ptr [rbp+40h],rax
  0000000140A21E10: movups      xmm0,xmmword ptr [rbp+0C0h]
  0000000140A21E17: movaps      xmmword ptr [rbp+30h],xmm0
  0000000140A21E1B: movups      xmm0,xmmword ptr [rbp+0B0h]
  0000000140A21E22: movaps      xmmword ptr [rbp+20h],xmm0
  0000000140A21E26: movups      xmm0,xmmword ptr [rbp+0A0h]
  0000000140A21E2D: movaps      xmmword ptr [rbp+10h],xmm0
  0000000140A21E31: movups      xmm0,xmmword ptr [rbp+60h]
  0000000140A21E35: movups      xmm1,xmmword ptr [rbp+70h]
  0000000140A21E39: movups      xmm2,xmmword ptr [rbp+80h]
  0000000140A21E40: movups      xmm3,xmmword ptr [rbp+90h]
  0000000140A21E47: movaps      xmmword ptr [rbp],xmm3
  0000000140A21E4B: movaps      xmmword ptr [rbp-10h],xmm2
  0000000140A21E4F: movaps      xmmword ptr [rbp-20h],xmm1
  0000000140A21E53: movaps      xmmword ptr [rbp-30h],xmm0
  0000000140A21E57: mov         qword ptr [rsp+20h],r13
  0000000140A21E5C: lea         rdx,[142AC9898h]
  0000000140A21E63: lea         rcx,[rbp-30h]
  0000000140A21E67: mov         r8d,0Ah
  0000000140A21E6D: mov         r9,rsi
  0000000140A21E70: call        _ZN12country_core7startup22load_mesh_into_library17h9943ce76e264ec52E
  0000000140A21E75: mov         rsi,rax
  0000000140A21E78: mov         rax,qword ptr [rbp+50h]
  0000000140A21E7C: mov         rcx,qword ptr [rax]
  0000000140A21E7F: add         rcx,10h
  0000000140A21E83: mov         rax,qword ptr [rbp+58h]
  0000000140A21E87: mov         qword ptr [rsp+30h],rax
  0000000140A21E8C: mov         rax,qword ptr [rbp+48h]
  0000000140A21E90: mov         qword ptr [rsp+28h],rax
  0000000140A21E95: mov         qword ptr [rsp+20h],1Bh
  0000000140A21E9E: lea         rdx,[142AC9820h]
  0000000140A21EA5: lea         r9,[142AC98A2h]
  0000000140A21EAC: mov         r8d,23h
  0000000140A21EB2: call        _ZN12country_core7startup24load_shader_into_library17haa83c8b937a2393cE
  0000000140A21EB7: mov         rbx,rax
  0000000140A21EBA: lea         rcx,[rbp+60h]
  0000000140A21EBE: mov         rdx,qword ptr [rbp+168h]
  0000000140A21EC5: call        _ZN8bevy_ecs6system8commands8Commands11spawn_empty17hdff45581613ea8c5E
  0000000140A21ECA: mov         rdi,qword ptr [rbp+68h]
  0000000140A21ECE: test        rdi,rdi
  0000000140A21ED1: jne         0000000140A21ED7
  0000000140A21ED3: mov         rdi,qword ptr [rbp+70h]
  0000000140A21ED7: mov         r15,qword ptr [rbp+80h]
  0000000140A21EDE: mov         rax,qword ptr [rdi]
  0000000140A21EE1: mov         r14,qword ptr [rdi+10h]
  0000000140A21EE5: sub         rax,r14
  0000000140A21EE8: cmp         rax,77h
  0000000140A21EEC: jbe         0000000140A21FBF
  0000000140A21EF2: mov         rax,qword ptr [rdi+8]
  0000000140A21EF6: lea         rcx,[_ZN4core3ops8function6FnOnce9call_once17hf29b79776c9d74aaE.llvm.11137708931887948641]
  0000000140A21EFD: mov         qword ptr [rax+r14],rcx
  0000000140A21F01: movups      xmmword ptr [rax+r14+8],xmm6
  0000000140A21F07: mov         qword ptr [rax+r14+18h],0
  0000000140A21F10: mov         dword ptr [rax+r14+20h],0
  0000000140A21F19: mov         rcx,3F8000003F800000h
  0000000140A21F23: mov         qword ptr [rax+r14+24h],rcx
  0000000140A21F28: mov         dword ptr [rax+r14+2Ch],3F800000h
  0000000140A21F31: mov         qword ptr [rax+r14+38h],rsi
  0000000140A21F36: mov         qword ptr [rax+r14+40h],rbx
  0000000140A21F3B: mov         byte ptr [rax+r14+48h],0
  0000000140A21F41: mov         qword ptr [rax+r14+58h],r15
  0000000140A21F46: lea         rcx,[_ZN8bevy_ecs5error7handler35panic$u7b$$u7b$reify.shim$u7d$$u7d$17hfabb946af0d30473E.llvm.15983634890098048951]
  0000000140A21F4D: mov         qword ptr [rax+r14+68h],rcx
  0000000140A21F52: add         r14,78h
  0000000140A21F56: mov         qword ptr [rdi+10h],r14
  0000000140A21F5A: movaps      xmm6,xmmword ptr [rbp+0E0h]
  0000000140A21F61: add         rsp,178h
  0000000140A21F68: pop         rbx
  0000000140A21F69: pop         rdi
  0000000140A21F6A: pop         rsi
  0000000140A21F6B: pop         r12
  0000000140A21F6D: pop         r13
  0000000140A21F6F: pop         r14
  0000000140A21F71: pop         r15
  0000000140A21F73: pop         rbp
  0000000140A21F74: ret
  0000000140A21F75: mov         qword ptr [rsp+20h],1
  0000000140A21F7E: mov         r8d,78h
  0000000140A21F84: mov         r9d,1
  0000000140A21F8A: mov         rcx,r14
  0000000140A21F8D: mov         rdx,r12
  0000000140A21F90: call        _ZN5alloc7raw_vec20RawVecInner$LT$A$GT$7reserve21do_reserve_and_handle17hdf5333d5fb0f60c2E
  0000000140A21F95: jmp         0000000140A21BFD
  0000000140A21F9A: mov         qword ptr [rsp+20h],1
  0000000140A21FA3: mov         r8d,78h
  0000000140A21FA9: mov         r9d,1
  0000000140A21FAF: mov         rcx,r15
  0000000140A21FB2: mov         rdx,r12
  0000000140A21FB5: call        _ZN5alloc7raw_vec20RawVecInner$LT$A$GT$7reserve21do_reserve_and_handle17hdf5333d5fb0f60c2E
  0000000140A21FBA: jmp         0000000140A21D7B
  0000000140A21FBF: mov         qword ptr [rsp+20h],1
  0000000140A21FC8: mov         r8d,78h
  0000000140A21FCE: mov         r9d,1
  0000000140A21FD4: mov         rcx,rdi
  0000000140A21FD7: mov         rdx,r14
  0000000140A21FDA: call        _ZN5alloc7raw_vec20RawVecInner$LT$A$GT$7reserve21do_reserve_and_handle17hdf5333d5fb0f60c2E
  0000000140A21FDF: jmp         0000000140A21EF2
  0000000140A21FE4: mov         rax,qword ptr [rbp+68h]
  0000000140A21FE8: mov         qword ptr [rbp-30h],rax
  0000000140A21FEC: lea         rax,[142AC9780h]
  0000000140A21FF3: mov         qword ptr [rsp+20h],rax
  0000000140A21FF8: lea         rcx,[142AC8AC0h]
  0000000140A21FFF: lea         r9,[142AC8AA0h]
  0000000140A22006: lea         r8,[rbp-30h]
  0000000140A2200A: mov         edx,2Bh
  0000000140A2200F: call        _ZN4core6result13unwrap_failed17h96d74ae09566f4b3E
  0000000140A22014: jmp         0000000140A22078
  0000000140A22016: mov         rax,qword ptr [rbp+68h]
  0000000140A2201A: mov         qword ptr [rbp-30h],rax
  0000000140A2201E: lea         rax,[142AC9800h]
  0000000140A22025: mov         qword ptr [rsp+20h],rax
  0000000140A2202A: lea         rcx,[142AC8AC0h]
  0000000140A22031: lea         r9,[142AC8AA0h]
  0000000140A22038: lea         r8,[rbp-30h]
  0000000140A2203C: mov         edx,2Bh
  0000000140A22041: call        _ZN4core6result13unwrap_failed17h96d74ae09566f4b3E
  0000000140A22046: jmp         0000000140A22078
  0000000140A22048: mov         rax,qword ptr [rbp+68h]
  0000000140A2204C: mov         qword ptr [rbp-30h],rax
  0000000140A22050: lea         rax,[142AC9880h]
  0000000140A22057: mov         qword ptr [rsp+20h],rax
  0000000140A2205C: lea         rcx,[142AC8AC0h]
  0000000140A22063: lea         r9,[142AC8AA0h]
  0000000140A2206A: lea         r8,[rbp-30h]
  0000000140A2206E: mov         edx,2Bh
  0000000140A22073: call        _ZN4core6result13unwrap_failed17h96d74ae09566f4b3E
  0000000140A22078: ud2
  0000000140A2207A: nop         word ptr [rax+rax]
  0000000140A22080: mov         qword ptr [rsp+10h],rdx
  0000000140A22085: push        rbp
  0000000140A22086: push        r15
  0000000140A22088: push        r14
  0000000140A2208A: push        r13
  0000000140A2208C: push        r12
  0000000140A2208E: push        rsi
  0000000140A2208F: push        rdi
  0000000140A22090: push        rbx
  0000000140A22091: sub         rsp,48h
  0000000140A22095: lea         rbp,[rdx+80h]
  0000000140A2209C: movaps      xmmword ptr [rsp+30h],xmm6
  0000000140A220A1: lea         rcx,[rbp-30h]
  0000000140A220A5: call        _ZN6anyhow5error65_$LT$impl$u20$core..ops..drop..Drop$u20$for$u20$anyhow..Error$GT$4drop17h2a52e76aa8e9c217E
  0000000140A220AA: movaps      xmm6,xmmword ptr [rsp+30h]
  0000000140A220AF: add         rsp,48h
  0000000140A220B3: pop         rbx
  0000000140A220B4: pop         rdi
  0000000140A220B5: pop         rsi
  0000000140A220B6: pop         r12
  0000000140A220B8: pop         r13
  0000000140A220BA: pop         r14
  0000000140A220BC: pop         r15
  0000000140A220BE: pop         rbp
  0000000140A220BF: ret
  0000000140A220C0: mov         qword ptr [rsp+10h],rdx
  0000000140A220C5: push        rbp
  0000000140A220C6: push        r15
  0000000140A220C8: push        r14
  0000000140A220CA: push        r13
  0000000140A220CC: push        r12
  0000000140A220CE: push        rsi
  0000000140A220CF: push        rdi
  0000000140A220D0: push        rbx
  0000000140A220D1: sub         rsp,48h
  0000000140A220D5: lea         rbp,[rdx+80h]
  0000000140A220DC: movaps      xmmword ptr [rsp+30h],xmm6
  0000000140A220E1: lea         rcx,[rbp-30h]
  0000000140A220E5: call        _ZN6anyhow5error65_$LT$impl$u20$core..ops..drop..Drop$u20$for$u20$anyhow..Error$GT$4drop17h2a52e76aa8e9c217E
  0000000140A220EA: movaps      xmm6,xmmword ptr [rsp+30h]
  0000000140A220EF: add         rsp,48h
  0000000140A220F3: pop         rbx
  0000000140A220F4: pop         rdi
  0000000140A220F5: pop         rsi
  0000000140A220F6: pop         r12
  0000000140A220F8: pop         r13
  0000000140A220FA: pop         r14
  0000000140A220FC: pop         r15
  0000000140A220FE: pop         rbp
  0000000140A220FF: ret
  0000000140A22100: mov         qword ptr [rsp+10h],rdx
  0000000140A22105: push        rbp
  0000000140A22106: push        r15
  0000000140A22108: push        r14
  0000000140A2210A: push        r13
  0000000140A2210C: push        r12
  0000000140A2210E: push        rsi
  0000000140A2210F: push        rdi
  0000000140A22110: push        rbx
  0000000140A22111: sub         rsp,48h
  0000000140A22115: lea         rbp,[rdx+80h]
  0000000140A2211C: movaps      xmmword ptr [rsp+30h],xmm6
  0000000140A22121: lea         rcx,[rbp-30h]
  0000000140A22125: call        _ZN6anyhow5error65_$LT$impl$u20$core..ops..drop..Drop$u20$for$u20$anyhow..Error$GT$4drop17h2a52e76aa8e9c217E
  0000000140A2212A: movaps      xmm6,xmmword ptr [rsp+30h]
  0000000140A2212F: add         rsp,48h
  0000000140A22133: pop         rbx
  0000000140A22134: pop         rdi
  0000000140A22135: pop         rsi
  0000000140A22136: pop         r12
  0000000140A22138: pop         r13
  0000000140A2213A: pop         r14
  0000000140A2213C: pop         r15
  0000000140A2213E: pop         rbp
  0000000140A2213F: ret

; ===== bevy_ecs::label::impl$0::as_any<bevy_ecs::schedule::set::SystemTypeSet<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> > >:
  00000001413F47C0: mov         rax,rcx
  00000001413F47C3: lea         rdx,[142C58480h]
  00000001413F47CA: ret
  00000001413F47CB: CC CC CC CC CC                                   .....

; ===== bevy_ecs::label::impl$0::as_any<bevy_ecs::schedule::set::SystemTypeSet<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> > >:
  00000001413F51C0: mov         rax,rcx
  00000001413F51C3: lea         rdx,[142C59880h]
  00000001413F51CA: ret
  00000001413F51CB: CC CC CC CC CC                                   .....

; ===== bevy_ecs::schedule::set::impl$7::as_dyn_eq<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> >:
  0000000141705C40: mov         rax,rcx
  0000000141705C43: lea         rdx,[142C93748h]
  0000000141705C4A: ret
  0000000141705C4B: CC CC CC CC CC                                   .....

; ===== bevy_ecs::schedule::set::impl$7::as_dyn_eq<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> >:
  0000000141706A60: mov         rax,rcx
  0000000141706A63: lea         rdx,[142C961A8h]
  0000000141706A6A: ret
  0000000141706A6B: CC CC CC CC CC                                   .....

; ===== bevy_ecs::label::impl$0::as_any<bevy_ecs::schedule::set::SystemTypeSet<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::event::reader::EventReader<country_core::systems::ivy::ivy_leaf_spawner::IvySegmentDoneGrowingMsg>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafStorage>,bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_leaf_storage::IvyLeafRng>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::time::GameTime>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> > >:
  000000014170A190: mov         rax,rcx
  000000014170A193: lea         rdx,[142C9B950h]
  000000014170A19A: ret
  000000014170A19B: CC CC CC CC CC                                   .....

; ===== bevy_ecs::label::impl$0::as_any<bevy_ecs::schedule::set::SystemTypeSet<bevy_ecs::system::function_system::FunctionSystem<void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>),void (*)(bevy_ecs::change_detection::ResMut<country_core::resources::ivy::ivy_storage::IvyStorage>,bevy_ecs::change_detection::Res<country_core::resources::ivy::ivy_direction_proposer::IvySpawnNoise>,bevy_ecs::change_detection::Res<country_core::resources::walls::public_walls::PublicWalls>,bevy_ecs::change_detection::Res<country_core::resources::cursor_terrain_raycast::CursorTerrainRaycast>,bevy_ecs::change_detection::Res<country_core::input::action_set::ActionSetStates>,bevy_ecs::change_detection::Res<country_core::systems::wall::wall_state::wall_spatial_hash::WallSpatialHash>,bevy_ecs::change_detection::Res<country_core::resources::glade_settings::GladeSettings>)> > >:
  000000014170AB90: mov         rax,rcx
  000000014170AB93: lea         rdx,[142C9CD50h]
  000000014170AB9A: ret
  000000014170AB9B: CC CC CC CC CC                                   .....
