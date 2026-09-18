; TinyGlade 窗变门逆向 · 反汇编证据摘录（2026-09-16）
; 源：dumpbin /DISASM:NOBYTES tiny-glade.exe（配套 tiny_glade.pdb），流式过滤后按地址区间摘录。
; 浮点常量 __real@xxxxxxxx 为单精度、单位米。报告：../TinyGlade_窗变门逆向_附录E.md

; ====================================================================================================
; DecoratorType 小函数：判别值 0x38 CottageWindow / 0x39 GothicWindow / 0x3A ArrowSlit / 0x3B Chimney / 0x3C Lantern / 0x3D Flag / 0x3F Stairs
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType8max_rank17h3ee0b0393a04172dE:  [140833130 .. 140833143]  (6 lines)
  0000000140833130: add         cl,0C8h
  0000000140833133: xor         eax,eax
  0000000140833135: cmp         cl,3
  0000000140833138: setb        al
  000000014083313B: lea         rax,[rax*2+1]
  0000000140833143: ret
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType9is_window17ha8fe327a00739f8aE:  [140833160 .. 140833169]  (4 lines)
  0000000140833160: add         cl,0C8h
  0000000140833163: cmp         cl,3
  0000000140833166: setb        al
  0000000140833169: ret
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType14spawns_clutter17hdcb2fdab3f05fa44E:  [140833170 .. 140833179]  (4 lines)
  0000000140833170: and         cl,3Eh
  0000000140833173: cmp         cl,38h
  0000000140833176: sete        al
  0000000140833179: ret
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType22can_place_on_stair_top17hac9011691cfee1c1E:  [140833190 .. 1408331A6]  (10 lines)
  0000000140833190: movzx       eax,byte ptr [rcx]
  0000000140833193: mov         ecx,eax
  0000000140833195: not         cl
  0000000140833197: test        cl,38h
  000000014083319A: setne       cl
  000000014083319D: add         al,0C4h
  000000014083319F: cmp         al,3
  00000001408331A1: setb        al
  00000001408331A4: or          al,cl
  00000001408331A6: ret
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType20can_place_on_terrain17hb48ba84f0e1ac92dE:  [1408331B0 .. 1408331BA]  (5 lines)
  00000001408331B0: movzx       eax,byte ptr [rcx]
  00000001408331B3: and         al,3Ch
  00000001408331B5: cmp         al,38h
  00000001408331B7: setne       al
  00000001408331BA: ret
; ---- _ZN12country_core9resources5walls9decorator13DecoratorType27should_offer_snap_to_corner17h305383b8c29d4956E:  [1408331C0 .. 1408331C6]  (3 lines)
  00000001408331C0: cmp         byte ptr [rcx],3Fh
  00000001408331C3: setne       al
  00000001408331C6: ret

; ====================================================================================================
; DecoratorSubtype::can_be_on_corner：位 3/12/16/19 = CottageCornerWindow/FlagCorner/LanternWall/ChimneyWall（门 4/9 不在内）
; ---- _ZN12country_core9resources5walls17decorator_subtype16DecoratorSubtype16can_be_on_corner17h4f27299ea9762f82E:  [140B25B50 .. 140B25B5E]  (5 lines)
  0000000140B25B50: movzx       eax,byte ptr [rcx]
  0000000140B25B53: mov         ecx,91008h
  0000000140B25B58: bt          ecx,eax
  0000000140B25B5B: setb        al
  0000000140B25B5E: ret

; ====================================================================================================
; impl Debug for DecoratorSubtype：长度表 0x142AE9EB0 + 相对偏移表 0x142AE9F80（26 个变体名，见报告 §3.1）
; ---- _ZN104_$LT$country_core..resources..walls..decorator_subtype..DecoratorSubtype$u20$as$u20$core..fmt..Debug$GT$3fmt17h6241620f5556f958E.llvm.17342129544673437732  [140B26E10 .. 140B26E32]  (9 lines)
  0000000140B26E10: mov         rax,rdx
  0000000140B26E13: movzx       ecx,byte ptr [rcx]
  0000000140B26E16: lea         rdx,[142AE9EB0h]
  0000000140B26E1D: mov         r8,qword ptr [rdx+rcx*8]
  0000000140B26E21: lea         r9,[142AE9F80h]
  0000000140B26E28: movsxd      rdx,dword ptr [r9+rcx*4]
  0000000140B26E2C: add         rdx,r9
  0000000140B26E2F: mov         rcx,rax
  0000000140B26E32: jmp         _ZN4core3fmt9Formatter9write_str17h47944a04c0e87e3bE

; ====================================================================================================
; derive_decorator_subtype：按 DecoratorType 跳表分派；窗挂墙分支 corner→platform.is_valid→is_half_dormer
; ---- _ZN16system_decorator24derive_decorator_subtype24derive_decorator_subtype17hf10600c0bc674851E:  [140813D00 .. 140813D57]  (25 lines)
  0000000140813D00: push        r15
  0000000140813D02: push        r14
  0000000140813D04: push        r13
  0000000140813D06: push        r12
  0000000140813D08: push        rsi
  0000000140813D09: push        rdi
  0000000140813D0A: push        rbx
  0000000140813D0B: sub         rsp,90h
  0000000140813D12: mov         rdi,r8
  0000000140813D15: mov         r15,qword ptr [rcx]
  0000000140813D18: mov         rax,qword ptr [rcx+8]
  0000000140813D1C: mov         r14,qword ptr [rcx+10h]
  0000000140813D20: mov         qword ptr [rsp+78h],rax
  0000000140813D25: mov         qword ptr [rsp+80h],r14
  0000000140813D2D: movzx       ecx,byte ptr [r15+46h]
  0000000140813D32: add         cl,0C8h
  0000000140813D35: cmp         cl,8
  0000000140813D38: movzx       ecx,cl
  0000000140813D3B: mov         r8d,6
  0000000140813D41: cmovb       r8d,ecx
  0000000140813D45: movzx       ecx,r8b
  0000000140813D49: lea         r8,[142A8F990h]
  0000000140813D50: movsxd      rcx,dword ptr [r8+rcx*4]
  0000000140813D54: add         rcx,r8
  0000000140813D57: jmp         rcx
; ---- _ZN16system_decorator24derive_decorator_subtype24derive_decorator_subtype17hf10600c0bc674851E:  [140814110 .. 140814134]  (11 lines)
  0000000140814110: xor         edi,edi
  0000000140814112: movzx       ecx,byte ptr [rsp+58h]
  0000000140814117: call        _ZN12country_core9resources5walls15wall_attachment20WallCornerAttachment11is_attached17hf2d19b432ee7ec35E
  000000014081411C: test        al,al
  000000014081411E: je          0000000140814277
  0000000140814124: xor         eax,eax
  0000000140814126: cmp         byte ptr [r15+46h],38h
  000000014081412B: setne       al
  000000014081412E: lea         eax,[rax+rax*2]
  0000000140814131: add         eax,3
  0000000140814134: jmp         00000001408142E3
; ---- _ZN16system_decorator24derive_decorator_subtype24derive_decorator_subtype17hf10600c0bc674851E:  [14081425C .. 1408142E0]  (36 lines)
  000000014081425C: xor         ecx,ecx
  000000014081425E: imul        rdi,rax,58h
  0000000140814262: add         rdi,rcx
  0000000140814265: movzx       ecx,byte ptr [rsp+58h]
  000000014081426A: call        _ZN12country_core9resources5walls15wall_attachment20WallCornerAttachment11is_attached17hf2d19b432ee7ec35E
  000000014081426F: test        al,al
  0000000140814271: jne         0000000140814124
  0000000140814277: lea         rcx,[rsp+40h]
  000000014081427C: call        _ZN12country_core9resources5walls6WallId8is_valid17ha6e55edfa838d994E
  0000000140814281: test        al,al
  0000000140814283: je          0000000140814297
  0000000140814285: xor         eax,eax
  0000000140814287: cmp         byte ptr [r15+46h],38h
  000000014081428C: setne       al
  000000014081428F: lea         eax,[rax+rax*4]
  0000000140814292: add         eax,4
  0000000140814295: jmp         00000001408142E3
  0000000140814297: movzx       ecx,byte ptr [r15+44h]
  000000014081429C: movzx       edx,byte ptr [r15+46h]
  00000001408142A1: mov         qword ptr [rsp+28h],rbx
  00000001408142A6: mov         qword ptr [rsp+20h],rdi
  00000001408142AB: lea         r8,[rsp+88h]
  00000001408142B3: mov         r9,rsi
  00000001408142B6: call        0000000140814470
  00000001408142BB: movzx       ecx,byte ptr [r15+46h]
  00000001408142C0: test        al,al
  00000001408142C2: je          00000001408142D6
  00000001408142C4: cmp         cl,38h
  00000001408142C7: mov         ecx,1
  00000001408142CC: mov         eax,7
  00000001408142D1: cmove       eax,ecx
  00000001408142D4: jmp         00000001408142E3
  00000001408142D6: xor         edx,edx
  00000001408142D8: cmp         cl,38h
  00000001408142DB: mov         eax,6
  00000001408142E0: cmove       eax,edx

; ====================================================================================================
; snap_balcony_door（全函数）：tag 0 阳台 / 1 落地 / 2 楼梯 / 3 无 / 4 太高
; ---- _ZN16system_decorator10ui_systems28decorator_interaction_intent17snap_balcony_door17hc584b6d3d4613812E:  [14127F0E0 .. 14127FDAC]  (715 lines)
  000000014127F0E0: push        r15
  000000014127F0E2: push        r14
  000000014127F0E4: push        r13
  000000014127F0E6: push        r12
  000000014127F0E8: push        rsi
  000000014127F0E9: push        rdi
  000000014127F0EA: push        rbp
  000000014127F0EB: push        rbx
  000000014127F0EC: sub         rsp,158h
  000000014127F0F3: movaps      xmmword ptr [rsp+140h],xmm15
  000000014127F0FC: movaps      xmmword ptr [rsp+130h],xmm14
  000000014127F105: movaps      xmmword ptr [rsp+120h],xmm13
  000000014127F10E: movaps      xmmword ptr [rsp+110h],xmm12
  000000014127F117: movaps      xmmword ptr [rsp+100h],xmm11
  000000014127F120: movaps      xmmword ptr [rsp+0F0h],xmm10
  000000014127F129: movaps      xmmword ptr [rsp+0E0h],xmm9
  000000014127F132: movaps      xmmword ptr [rsp+0D0h],xmm8
  000000014127F13B: movaps      xmmword ptr [rsp+0C0h],xmm7
  000000014127F143: movaps      xmmword ptr [rsp+0B0h],xmm6
  000000014127F14B: mov         r14,r9
  000000014127F14E: movzx       esi,byte ptr [r8+46h]
  000000014127F153: mov         al,3
  000000014127F155: cmp         esi,38h
  000000014127F158: je          000000014127F170
  000000014127F15A: cmp         esi,39h
  000000014127F15D: jne         000000014127FCEC
  000000014127F163: mov         byte ptr [rsp+30h],dl
  000000014127F167: mov         qword ptr [rsp+38h],rcx
  000000014127F16C: mov         dl,9
  000000014127F16E: jmp         000000014127F17B
  000000014127F170: mov         byte ptr [rsp+30h],dl
  000000014127F174: mov         qword ptr [rsp+38h],rcx
  000000014127F179: mov         dl,4
  000000014127F17B: mov         rbx,qword ptr [rsp+1F8h]
  000000014127F183: movzx       edi,byte ptr [r8+44h]
  000000014127F188: mov         ecx,edi
  000000014127F18A: call        _ZN12country_core9resources5walls17decorator_subtype15DecoratorLibKey3new17hce67db27b83b546aE
  000000014127F18F: mov         byte ptr [rsp+2Eh],al
  000000014127F193: mov         byte ptr [rsp+2Fh],dl
  000000014127F197: cmp         qword ptr [rbx+18h],0
  000000014127F19C: je          000000014127F2E9
  000000014127F1A2: lea         r15,[rbx+20h]
  000000014127F1A6: lea         rdx,[rsp+2Eh]
  000000014127F1AB: mov         rcx,r15
  000000014127F1AE: call        _ZN4core4hash11BuildHasher8hash_one17h119cf64593421c3cE
  000000014127F1B3: mov         r12,qword ptr [rbx]
  000000014127F1B6: mov         r13,qword ptr [rbx+8]
  000000014127F1BA: mov         rcx,r13
  000000014127F1BD: and         rcx,rax
  000000014127F1C0: shr         rax,39h
  000000014127F1C4: movd        xmm0,eax
  000000014127F1C8: punpcklbw   xmm0,xmm0
  000000014127F1CC: pshuflw     xmm0,xmm0,0
  000000014127F1D1: pshufd      xmm0,xmm0,0
  000000014127F1D6: lea         rbx,[r12-48h]
  000000014127F1DB: movzx       eax,byte ptr [rsp+2Eh]
  000000014127F1E0: cmp         al,1Ah
  000000014127F1E2: jne         000000014127F24A
  000000014127F1E4: xor         eax,eax
  000000014127F1E6: pcmpeqd     xmm1,xmm1
  000000014127F1EA: movdqu      xmm2,xmmword ptr [r12+rcx]
  000000014127F1F0: movdqa      xmm3,xmm2
  000000014127F1F4: pcmpeqb     xmm3,xmm0
  000000014127F1F8: pmovmskb    edx,xmm3
  000000014127F1FC: test        edx,edx
  000000014127F1FE: je          000000014127F22A
  000000014127F200: tzcnt       r10d,edx
  000000014127F205: add         r10,rcx
  000000014127F208: and         r10,r13
  000000014127F20B: neg         r10
  000000014127F20E: lea         r8,[r10+r10*8]
  000000014127F212: cmp         byte ptr [rbx+r8*8],1Ah
  000000014127F217: je          000000014127F2C5
  000000014127F21D: lea         r8d,[rdx-1]
  000000014127F221: and         r8w,dx
  000000014127F225: mov         edx,r8d
  000000014127F228: jne         000000014127F200
  000000014127F22A: pcmpeqb     xmm2,xmm1
  000000014127F22E: pmovmskb    edx,xmm2
  000000014127F232: test        edx,edx
  000000014127F234: jne         000000014127F2E9
  000000014127F23A: add         rcx,rax
  000000014127F23D: add         rcx,10h
  000000014127F241: add         rax,10h
  000000014127F245: and         rcx,r13
  000000014127F248: jmp         000000014127F1EA
  000000014127F24A: movzx       edx,byte ptr [rsp+2Fh]
  000000014127F24F: xor         r8d,r8d
  000000014127F252: pcmpeqd     xmm1,xmm1
  000000014127F256: movdqu      xmm2,xmmword ptr [r12+rcx]
  000000014127F25C: movdqa      xmm3,xmm2
  000000014127F260: pcmpeqb     xmm3,xmm0
  000000014127F264: pmovmskb    r9d,xmm3
  000000014127F269: test        r9d,r9d
  000000014127F26C: je          000000014127F2A7
  000000014127F26E: tzcnt       r10d,r9d
  000000014127F273: add         r10,rcx
  000000014127F276: and         r10,r13
  000000014127F279: neg         r10
  000000014127F27C: lea         r11,[r10+r10*8]
  000000014127F280: movzx       ebp,byte ptr [rbx+r11*8]
  000000014127F285: cmp         bpl,1Ah
  000000014127F289: je          000000014127F29A
  000000014127F28B: cmp         al,bpl
  000000014127F28E: jne         000000014127F29A
  000000014127F290: lea         r11,[rbx+r11*8]
  000000014127F294: cmp         dl,byte ptr [r11+1]
  000000014127F298: je          000000014127F2C5
  000000014127F29A: lea         r10d,[r9-1]
  000000014127F29E: and         r10w,r9w
  000000014127F2A2: mov         r9d,r10d
  000000014127F2A5: jne         000000014127F26E
  000000014127F2A7: pcmpeqb     xmm2,xmm1
  000000014127F2AB: pmovmskb    r9d,xmm2
  000000014127F2B0: test        r9d,r9d
  000000014127F2B3: jne         000000014127F2E9
  000000014127F2B5: add         rcx,r8
  000000014127F2B8: add         rcx,10h
  000000014127F2BC: add         r8,10h
  000000014127F2C0: and         rcx,r13
  000000014127F2C3: jmp         000000014127F256
  000000014127F2C5: lea         rax,[r10+r10*8]
  000000014127F2C9: movss       xmm13,dword ptr [r12+rax*8-4]
  000000014127F2D0: subss       xmm13,dword ptr [r12+rax*8-8]
  000000014127F2D7: cmp         esi,38h
  000000014127F2DA: je          000000014127F2FB
  000000014127F2DC: cmp         esi,39h
  000000014127F2DF: jne         000000014127F94B
  000000014127F2E5: mov         dl,6
  000000014127F2E7: jmp         000000014127F2FD
  000000014127F2E9: lea         rax,[rsp+2Eh]
  000000014127F2EE: mov         qword ptr [rsp+88h],rax
  000000014127F2F6: jmp         000000014127F95F
  000000014127F2FB: xor         edx,edx
  000000014127F2FD: mov         ecx,edi
  000000014127F2FF: call        _ZN12country_core9resources5walls17decorator_subtype15DecoratorLibKey3new17hce67db27b83b546aE
  000000014127F304: mov         byte ptr [rsp+2Eh],al
  000000014127F308: mov         byte ptr [rsp+2Fh],dl
  000000014127F30C: lea         rdi,[rsp+2Eh]
  000000014127F311: mov         rcx,r15
  000000014127F314: mov         rdx,rdi
  000000014127F317: call        _ZN4core4hash11BuildHasher8hash_one17h119cf64593421c3cE
  000000014127F31C: mov         rcx,rax
  000000014127F31F: shr         rcx,39h
  000000014127F323: and         rax,r13
  000000014127F326: movd        xmm0,ecx
  000000014127F32A: punpcklbw   xmm0,xmm0
  000000014127F32E: pshuflw     xmm0,xmm0,0
  000000014127F333: pshufd      xmm0,xmm0,0
  000000014127F338: movzx       ecx,byte ptr [rsp+2Eh]
  000000014127F33D: cmp         cl,1Ah
  000000014127F340: jne         000000014127F3A8
  000000014127F342: xor         ecx,ecx
  000000014127F344: pcmpeqd     xmm1,xmm1
  000000014127F348: movdqu      xmm2,xmmword ptr [r12+rax]
  000000014127F34E: movdqa      xmm3,xmm2
  000000014127F352: pcmpeqb     xmm3,xmm0
  000000014127F356: pmovmskb    edx,xmm3
  000000014127F35A: test        edx,edx
  000000014127F35C: je          000000014127F388
  000000014127F35E: tzcnt       r9d,edx
  000000014127F363: add         r9,rax
  000000014127F366: and         r9,r13
  000000014127F369: neg         r9
  000000014127F36C: lea         r8,[r9+r9*8]
  000000014127F370: cmp         byte ptr [rbx+r8*8],1Ah
  000000014127F375: je          000000014127F427
  000000014127F37B: lea         r8d,[rdx-1]
  000000014127F37F: and         r8w,dx
  000000014127F383: mov         edx,r8d
  000000014127F386: jne         000000014127F35E
  000000014127F388: pcmpeqb     xmm2,xmm1
  000000014127F38C: pmovmskb    edx,xmm2
  000000014127F390: test        edx,edx
  000000014127F392: jne         000000014127F957
  000000014127F398: add         rax,rcx
  000000014127F39B: add         rax,10h
  000000014127F39F: add         rcx,10h
  000000014127F3A3: and         rax,r13
  000000014127F3A6: jmp         000000014127F348
  000000014127F3A8: movzx       edx,byte ptr [rsp+2Fh]
  000000014127F3AD: xor         r8d,r8d
  000000014127F3B0: pcmpeqd     xmm1,xmm1
  000000014127F3B4: movdqu      xmm2,xmmword ptr [r12+rax]
  000000014127F3BA: movdqa      xmm3,xmm2
  000000014127F3BE: pcmpeqb     xmm3,xmm0
  000000014127F3C2: pmovmskb    r10d,xmm3
  000000014127F3C7: test        r10d,r10d
  000000014127F3CA: je          000000014127F405
  000000014127F3CC: tzcnt       r9d,r10d
  000000014127F3D1: add         r9,rax
  000000014127F3D4: and         r9,r13
  000000014127F3D7: neg         r9
  000000014127F3DA: lea         r11,[r9+r9*8]
  000000014127F3DE: movzx       esi,byte ptr [rbx+r11*8]
  000000014127F3E3: cmp         sil,1Ah
  000000014127F3E7: je          000000014127F3F8
  000000014127F3E9: cmp         cl,sil
  000000014127F3EC: jne         000000014127F3F8
  000000014127F3EE: lea         r11,[rbx+r11*8]
  000000014127F3F2: cmp         dl,byte ptr [r11+1]
  000000014127F3F6: je          000000014127F427
  000000014127F3F8: lea         r9d,[r10-1]
  000000014127F3FC: and         r9w,r10w
  000000014127F400: mov         r10d,r9d
  000000014127F403: jne         000000014127F3CC
  000000014127F405: pcmpeqb     xmm2,xmm1
  000000014127F409: pmovmskb    r9d,xmm2
  000000014127F40E: test        r9d,r9d
  000000014127F411: jne         000000014127F957
  000000014127F417: add         rax,r8
  000000014127F41A: add         rax,10h
  000000014127F41E: add         r8,10h
  000000014127F422: and         rax,r13
  000000014127F425: jmp         000000014127F3B4
  000000014127F427: mov         rax,qword ptr [rsp+1F0h]
  000000014127F42F: movss       xmm0,dword ptr [rsp+1C8h]
  000000014127F438: movss       xmm15,dword ptr [rsp+1C0h]
  000000014127F442: lea         rcx,[r9+r9*8]
  000000014127F446: movss       xmm12,dword ptr [r12+rcx*8-4]
  000000014127F44D: subss       xmm12,dword ptr [r12+rcx*8-8]
  000000014127F454: movss       xmm7,dword ptr [r14]
  000000014127F459: movss       xmm9,dword ptr [r14+4]
  000000014127F45F: movss       xmm1,dword ptr [r14+8]
  000000014127F465: movss       xmm8,dword ptr [__real@3e4ccccd]
  000000014127F46E: movaps      xmm11,xmm15
  000000014127F472: mulss       xmm11,xmm8
  000000014127F477: mulss       xmm8,xmm0
  000000014127F47C: addss       xmm11,xmm7
  000000014127F481: movaps      xmmword ptr [rsp+0A0h],xmm1
  000000014127F489: addss       xmm8,xmm1
  000000014127F48E: mov         rcx,qword ptr [rax]
  000000014127F491: mov         r12,qword ptr [rax+8]
  000000014127F495: mov         r14,qword ptr [rcx+0F8h]
  000000014127F49C: mov         rax,qword ptr [rcx+100h]
  000000014127F4A3: lea         rdi,[r14+rax*4]
  000000014127F4A7: mov         qword ptr [rsp+78h],rcx
  000000014127F4AC: movzx       r13d,byte ptr [rcx+108h]
  000000014127F4B4: mulss       xmm13,dword ptr [__real@3f000000]
  000000014127F4BD: movss       dword ptr [rsp+34h],xmm9
  000000014127F4C4: subss       xmm9,xmm13
  000000014127F4C9: movss       xmm6,dword ptr [__real@becccccd]
  000000014127F4D1: addss       xmm6,xmm9
  000000014127F4D6: mov         eax,8
  000000014127F4DB: mov         qword ptr [rsp+80h],rax
  000000014127F4E3: xor         esi,esi
  000000014127F4E5: movaps      xmm14,xmm6
  000000014127F4E9: cmpunordss  xmm14,xmm6
  000000014127F4EF: xor         ebx,ebx
  000000014127F4F1: xor         ebp,ebp
  000000014127F4F3: test        r13b,r13b
  000000014127F4F6: je          000000014127F580
  000000014127F4FC: cmp         rbp,rbx
  000000014127F4FF: jne         000000014127F60D
  000000014127F505: nop         word ptr cs:[rax+rax]
  000000014127F510: cmp         r14,rdi
  000000014127F513: je          000000014127F7C3
  000000014127F519: mov         ecx,dword ptr [r14]
  000000014127F51C: add         r14,4
  000000014127F520: mov         rax,qword ptr [r12+1A8h]
  000000014127F528: lea         rcx,[rcx+rcx*8]
  000000014127F52C: mov         rbx,qword ptr [rax+rcx*8+10h]
  000000014127F531: test        rbx,rbx
  000000014127F534: je          000000014127F510
  000000014127F536: lea         rax,[rax+rcx*8]
  000000014127F53A: mov         rcx,qword ptr [rsp+78h]
  000000014127F53F: mov         rcx,qword ptr [rcx+110h]
  000000014127F546: cmp         qword ptr [rax+40h],rcx
  000000014127F54A: jbe         000000014127F609
  000000014127F550: mov         rdx,qword ptr [rax+38h]
  000000014127F554: mov         rcx,qword ptr [rdx+rcx*8]
  000000014127F558: test        rcx,rcx
  000000014127F55B: je          000000014127F609
  000000014127F561: mov         rax,qword ptr [rax+18h]
  000000014127F565: not         rcx
  000000014127F568: lea         rcx,[rcx+rcx*2]
  000000014127F56C: shl         rcx,4
  000000014127F570: mov         rsi,qword ptr [rax+rcx+10h]
  000000014127F575: jmp         000000014127F60B
  000000014127F57A: nop         word ptr [rax+rax]
  000000014127F580: cmp         rbp,rbx
  000000014127F583: jne         000000014127F621
  000000014127F589: nop         dword ptr [rax]
  000000014127F590: cmp         r14,rdi
  000000014127F593: je          000000014127F7C3
  000000014127F599: mov         ecx,dword ptr [r14]
  000000014127F59C: add         r14,4
  000000014127F5A0: mov         rax,qword ptr [r12+100h]
  000000014127F5A8: lea         rcx,[rcx+rcx*4]
  000000014127F5AC: shl         rcx,5
  000000014127F5B0: mov         rbx,qword ptr [rax+rcx+58h]
  000000014127F5B5: test        rbx,rbx
  000000014127F5B8: je          000000014127F590
  000000014127F5BA: add         rax,rcx
  000000014127F5BD: mov         ecx,dword ptr [rax+94h]
  000000014127F5C3: mov         rdx,qword ptr [r12+1A8h]
  000000014127F5CB: lea         r8,[rcx+rcx*8]
  000000014127F5CF: mov         rcx,qword ptr [rsp+78h]
  000000014127F5D4: mov         rcx,qword ptr [rcx+110h]
  000000014127F5DB: cmp         qword ptr [rdx+r8*8+40h],rcx
  000000014127F5E0: jbe         000000014127F611
  000000014127F5E2: lea         rdx,[rdx+r8*8]
  000000014127F5E6: mov         r8,qword ptr [rdx+38h]
  000000014127F5EA: mov         rcx,qword ptr [r8+rcx*8]
  000000014127F5EE: test        rcx,rcx
  000000014127F5F1: je          000000014127F611
  000000014127F5F3: mov         rdx,qword ptr [rdx+18h]
  000000014127F5F7: not         rcx
  000000014127F5FA: lea         rcx,[rcx+rcx*2]
  000000014127F5FE: shl         rcx,4
  000000014127F602: mov         rsi,qword ptr [rdx+rcx+10h]
  000000014127F607: jmp         000000014127F613
  000000014127F609: xor         esi,esi
  000000014127F60B: xor         ebp,ebp
  000000014127F60D: mov         eax,ebp
  000000014127F60F: jmp         000000014127F634
  000000014127F611: xor         esi,esi
  000000014127F613: mov         rax,qword ptr [rax+50h]
  000000014127F617: mov         qword ptr [rsp+80h],rax
  000000014127F61F: xor         ebp,ebp
  000000014127F621: mov         rax,rbp
  000000014127F624: shl         rax,4
  000000014127F628: mov         rcx,qword ptr [rsp+80h]
  000000014127F630: mov         eax,dword ptr [rcx+rax+8]
  000000014127F634: imul        r15,rax,58h
  000000014127F638: add         r15,rsi
  000000014127F63B: inc         rbp
  000000014127F63E: mov         rcx,r15
  000000014127F641: call        _ZN12country_core9resources5roofs10roof_shape4Roof7is_flat17hf1ba4121fb5a18d9E
  000000014127F646: test        al,al
  000000014127F648: lea         rcx,[rsp+40h]
  000000014127F64D: movd        xmm3,dword ptr [__real@3f4ccccd]
  000000014127F655: je          000000014127F4F3
  000000014127F65B: mov         rax,qword ptr [rsp+1E0h]
  000000014127F663: cmp         qword ptr [r15],rax
  000000014127F666: je          000000014127F4F3
  000000014127F66C: movss       xmm10,dword ptr [r15+4Ch]
  000000014127F672: addss       xmm10,dword ptr [__real@bf4a3d71]
  000000014127F67B: mov         rax,qword ptr [rsp+1E8h]
  000000014127F683: movss       xmm0,dword ptr [rax+280h]
  000000014127F68B: addss       xmm0,dword ptr [__real@becccccd]
  000000014127F693: movss       xmm1,dword ptr [rsp+34h]
  000000014127F699: ucomiss     xmm1,xmm10
  000000014127F69D: jbe         000000014127F4F3
  000000014127F6A3: movaps      xmm1,xmm14
  000000014127F6A7: andps       xmm1,xmm0
  000000014127F6AA: maxss       xmm0,xmm6
  000000014127F6AE: movaps      xmm2,xmm14
  000000014127F6B2: andnps      xmm2,xmm0
  000000014127F6B5: orps        xmm2,xmm1
  000000014127F6B8: ucomiss     xmm10,xmm2
  000000014127F6BC: jb          000000014127F4F3
  000000014127F6C2: lea         rax,[r15+0Ch]
  000000014127F6C6: test        byte ptr [r15+8],1
  000000014127F6CB: je          000000014127F72B
  000000014127F6CD: mov         rdx,qword ptr [rax+10h]
  000000014127F6D1: mov         qword ptr [rsp+50h],rdx
  000000014127F6D6: movups      xmm0,xmmword ptr [rax]
  000000014127F6D9: movaps      xmmword ptr [rsp+40h],xmm0
  000000014127F6DE: mov         dword ptr [rsp+20h],3F4CCCCDh
  000000014127F6E6: movaps      xmm1,xmm7
  000000014127F6E9: movdqa      xmm2,xmmword ptr [rsp+0A0h]
  000000014127F6F2: call        _ZN5utils8geometry9rectangle11Rectangle2d22contains_point_padding17h6b03e3deb41d2716E
  000000014127F6F7: test        al,al
  000000014127F6F9: je          000000014127F4F3
  000000014127F6FF: mov         dword ptr [rsp+20h],3F4CCCCDh
  000000014127F707: lea         rcx,[rsp+40h]
  000000014127F70C: movaps      xmm1,xmm11
  000000014127F710: movaps      xmm2,xmm8
  000000014127F714: movd        xmm3,dword ptr [__real@3f4ccccd]
  000000014127F71C: call        _ZN5utils8geometry9rectangle11Rectangle2d22contains_point_padding17h6b03e3deb41d2716E
  000000014127F721: test        al,al
  000000014127F723: je          000000014127F4F3
  000000014127F729: jmp         000000014127F773
  000000014127F72B: movups      xmm0,xmmword ptr [rax]
  000000014127F72E: movaps      xmmword ptr [rsp+40h],xmm0
  000000014127F733: movaps      xmm1,xmm7
  000000014127F736: movdqa      xmm2,xmmword ptr [rsp+0A0h]
  000000014127F73F: call        _ZN5utils8geometry6circle8Circle2d15signed_distance17h70ddda9f493d9e93E
  000000014127F744: movss       xmm1,dword ptr [__real@3ecccccd]
  000000014127F74C: ucomiss     xmm1,xmm0
  000000014127F74F: jbe         000000014127F4F3
  000000014127F755: lea         rcx,[rsp+40h]
  000000014127F75A: movaps      xmm1,xmm11
  000000014127F75E: movaps      xmm2,xmm8
  000000014127F762: call        _ZN5utils8geometry6circle8Circle2d15signed_distance17h70ddda9f493d9e93E
  000000014127F767: xorps       xmm1,xmm1
  000000014127F76A: ucomiss     xmm1,xmm0
  000000014127F76D: jbe         000000014127F4F3
  000000014127F773: mov         rbp,qword ptr [r15]
  000000014127F776: mov         rdi,qword ptr [rsp+1E8h]
  000000014127F77E: movss       xmm0,dword ptr [rdi+280h]
  000000014127F786: movaps      xmm11,xmm10
  000000014127F78A: cmpunordss  xmm11,xmm10
  000000014127F790: movaps      xmm1,xmm11
  000000014127F794: andps       xmm1,xmm0
  000000014127F797: maxss       xmm0,xmm10
  000000014127F79C: andnps      xmm11,xmm0
  000000014127F7A0: orps        xmm11,xmm1
  000000014127F7A4: addss       xmm11,xmm13
  000000014127F7A9: xor         ebx,ebx
  000000014127F7AB: mov         rcx,qword ptr [rsp+38h]
  000000014127F7B0: movzx       eax,byte ptr [rsp+30h]
  000000014127F7B5: movss       xmm6,dword ptr [rsp+1D0h]
  000000014127F7BE: jmp         000000014127FA47
  000000014127F7C3: mov         rdi,qword ptr [rsp+1E8h]
  000000014127F7CB: mov         rax,qword ptr [rdi+98h]
  000000014127F7D2: cmp         rax,1
  000000014127F7D6: je          000000014127FBCB
  000000014127F7DC: test        rax,rax
  000000014127F7DF: je          000000014127FD5E
  000000014127F7E5: xorps       xmm0,xmm0
  000000014127F7E8: movss       xmm6,dword ptr [rsp+1D0h]
  000000014127F7F1: movaps      xmm1,xmm6
  000000014127F7F4: maxss       xmm1,xmm0
  000000014127F7F8: mov         rcx,qword ptr [rdi+90h]
  000000014127F7FF: mov         qword ptr [rsp+40h],rcx
  000000014127F804: mov         qword ptr [rsp+48h],rax
  000000014127F809: lea         rax,[_ZN4core3ops8function6FnOnce9call_once17h4daf489b02095387E.llvm.12921390545346410854]
  000000014127F810: mov         qword ptr [rsp+50h],rax
  000000014127F815: xorps       xmm0,xmm0
  000000014127F818: movups      xmmword ptr [rsp+58h],xmm0
  000000014127F81D: lea         rcx,[rsp+40h]
  000000014127F822: call        _ZN5utils5curve11curve_slice53CurveSlice$LT$T$C$Points$C$CSem$C$RemapT$C$RemapF$GT$18extend_right_until17h53e09b3ad41649deE
  000000014127F827: mov         ecx,dword ptr [rsp+60h]
  000000014127F82B: mov         rdx,qword ptr [rdi+98h]
  000000014127F832: cmp         rdx,rcx
  000000014127F835: mov         rsi,qword ptr [rsp+38h]
  000000014127F83A: jbe         000000014127FD76
  000000014127F840: lea         rax,[rcx+1]
  000000014127F844: cmp         rax,rdx
  000000014127F847: jae         000000014127FD82
  000000014127F84D: mov         rdx,qword ptr [rdi+90h]
  000000014127F854: lea         rcx,[rcx+rcx*2]
  000000014127F858: movss       xmm0,dword ptr [rdx+rcx*4+4]
  000000014127F85E: lea         rax,[rax+rax*2]
  000000014127F862: movss       xmm11,dword ptr [rdx+rax*4+4]
  000000014127F869: subss       xmm11,xmm0
  000000014127F86E: mulss       xmm11,dword ptr [rsp+64h]
  000000014127F875: addss       xmm11,xmm0
  000000014127F87A: xorps       xmm0,xmm0
  000000014127F87D: maxss       xmm11,xmm0
  000000014127F882: cmp         byte ptr [rsp+200h],0
  000000014127F88A: je          000000014127F9B3
  000000014127F890: movss       xmm0,dword ptr [__real@bdcccccd]
  000000014127F898: addss       xmm0,xmm9
  000000014127F89D: ucomiss     xmm11,xmm0
  000000014127F8A1: ja          000000014127F9D1
  000000014127F8A7: movss       xmm0,dword ptr [rsp+1C8h]
  000000014127F8B0: unpcklps    xmm15,xmm0
  000000014127F8B4: mulps       xmm15,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  000000014127F8BC: unpcklps    xmm7,xmmword ptr [rsp+0A0h]
  000000014127F8C4: addps       xmm7,xmm15
  000000014127F8C8: movlps      qword ptr [rsp+40h],xmm7
  000000014127F8CD: addss       xmm9,dword ptr [__real@3f000000]
  000000014127F8D6: lea         r14,[rsp+88h]
  000000014127F8DE: lea         rdx,[rsp+40h]
  000000014127F8E3: mov         rcx,r14
  000000014127F8E6: movaps      xmm2,xmm9
  000000014127F8EA: call        _ZN71_$LT$glam..f32..vec2..Vec2$u20$as$u20$utils..swizzlers..SwizzlerXVY$GT$3xvy17hcc482e0e27c4a762E
  000000014127F8EF: movsd       xmm0,mmword ptr [__xmm@0000000000000000bf80000080000000]
  000000014127F8F7: movsd       mmword ptr [rsp+94h],xmm0
  000000014127F900: mov         dword ptr [rsp+9Ch],80000000h
  000000014127F90B: mov         dword ptr [rsp+20h],1000h
  000000014127F913: lea         rcx,[rsp+40h]
  000000014127F918: movd        xmm3,dword ptr [__real@3f800000]
  000000014127F920: mov         rdx,qword ptr [rsp+210h]
  000000014127F928: mov         r8,r14
  000000014127F92B: call        _ZN12country_core7systems9collision5world12RaycastWorld14raycast_simple17hf5f9373685c2274fE
  000000014127F930: movzx       eax,byte ptr [rsp+40h]
  000000014127F935: cmp         eax,0Dh
  000000014127F938: je          000000014127FA01
  000000014127F93E: cmp         eax,11h
  000000014127F941: mov         rcx,rsi
  000000014127F944: mov         al,3
  000000014127F946: jmp         000000014127FCEC
  000000014127F94B: mov         rcx,qword ptr [rsp+38h]
  000000014127F950: mov         al,3
  000000014127F952: jmp         000000014127FCEC
  000000014127F957: mov         qword ptr [rsp+88h],rdi
  000000014127F95F: lea         rax,[14127CCF0h]
  000000014127F966: mov         qword ptr [rsp+90h],rax
  000000014127F96E: lea         rax,[142C242C0h]
  000000014127F975: mov         qword ptr [rsp+40h],rax
  000000014127F97A: mov         qword ptr [rsp+48h],1
  000000014127F983: mov         qword ptr [rsp+60h],0
  000000014127F98C: lea         rax,[rsp+88h]
  000000014127F994: mov         qword ptr [rsp+50h],rax
  000000014127F999: mov         qword ptr [rsp+58h],1
  000000014127F9A2: lea         rdx,[142C242D0h]
  000000014127F9A9: lea         rcx,[rsp+40h]
  000000014127F9AE: call        _ZN4core9panicking9panic_fmt17h57d10e7f426973d3E
  000000014127F9B3: mulss       xmm12,dword ptr [__real@bf000000]
  000000014127F9BC: addss       xmm12,dword ptr [rsp+34h]
  000000014127F9C3: ucomiss     xmm11,xmm12
  000000014127F9C7: movaps      xmm9,xmm12
  000000014127F9CB: jbe         000000014127F8A7
  000000014127F9D1: addss       xmm11,xmm13
  000000014127F9D6: addss       xmm11,dword ptr [__real@3d4ccccd]
  000000014127F9DF: movaps      xmm0,xmm13
  000000014127F9E3: maxss       xmm0,xmm11
  000000014127F9E8: cmpunordss  xmm11,xmm11
  000000014127F9EE: movaps      xmm1,xmm11
  000000014127F9F2: andnps      xmm1,xmm0
  000000014127F9F5: andps       xmm11,xmm13
  000000014127F9F9: orps        xmm11,xmm1
  000000014127F9FD: mov         bl,1
  000000014127F9FF: jmp         000000014127FA37
  000000014127FA01: movss       xmm11,dword ptr [rsp+5Ch]
  000000014127FA08: addss       xmm11,xmm13
  000000014127FA0D: addss       xmm11,dword ptr [__real@3d4ccccd]
  000000014127FA16: movaps      xmm0,xmm13
  000000014127FA1A: maxss       xmm0,xmm11
  000000014127FA1F: cmpunordss  xmm11,xmm11
  000000014127FA25: movaps      xmm1,xmm11
  000000014127FA29: andps       xmm1,xmm13
  000000014127FA2D: andnps      xmm11,xmm0
  000000014127FA31: orps        xmm11,xmm1
  000000014127FA35: mov         bl,2
  000000014127FA37: mov         rbp,qword ptr [rsp+1E0h]
  000000014127FA3F: mov         rcx,rsi
  000000014127FA42: movzx       eax,byte ptr [rsp+30h]
  000000014127FA47: test        al,al
  000000014127FA49: je          000000014127FCE1
  000000014127FA4F: mov         rax,qword ptr [rdi+98h]
  000000014127FA56: cmp         rax,1
  000000014127FA5A: je          000000014127FBCB
  000000014127FA60: test        rax,rax
  000000014127FA63: je          000000014127FD5E
  000000014127FA69: mov         rsi,rcx
  000000014127FA6C: xorps       xmm0,xmm0
  000000014127FA6F: maxss       xmm6,xmm0
  000000014127FA73: mov         rcx,qword ptr [rdi+90h]
  000000014127FA7A: mov         qword ptr [rsp+40h],rcx
  000000014127FA7F: mov         qword ptr [rsp+48h],rax
  000000014127FA84: lea         rax,[_ZN4core3ops8function6FnOnce9call_once17h4daf489b02095387E.llvm.12921390545346410854]
  000000014127FA8B: mov         qword ptr [rsp+50h],rax
  000000014127FA90: xorps       xmm0,xmm0
  000000014127FA93: movups      xmmword ptr [rsp+58h],xmm0
  000000014127FA98: lea         rcx,[rsp+40h]
  000000014127FA9D: movaps      xmm1,xmm6
  000000014127FAA0: call        _ZN5utils5curve11curve_slice53CurveSlice$LT$T$C$Points$C$CSem$C$RemapT$C$RemapF$GT$18extend_right_until17h53e09b3ad41649deE
  000000014127FAA5: mov         ecx,dword ptr [rsp+60h]
  000000014127FAA9: mov         rdx,qword ptr [rdi+0E8h]
  000000014127FAB0: cmp         rdx,rcx
  000000014127FAB3: jbe         000000014127FD91
  000000014127FAB9: mov         dword ptr [rsp+30h],ebx
  000000014127FABD: mov         qword ptr [rsp+38h],rsi
  000000014127FAC2: lea         rax,[rcx+1]
  000000014127FAC6: cmp         rax,rdx
  000000014127FAC9: jae         000000014127FD9D
  000000014127FACF: mov         rax,qword ptr [rdi+0E0h]
  000000014127FAD6: movss       xmm0,dword ptr [rax+rcx*4]
  000000014127FADB: movss       xmm6,dword ptr [rax+rcx*4+4]
  000000014127FAE1: subss       xmm6,xmm0
  000000014127FAE5: mulss       xmm6,dword ptr [rsp+64h]
  000000014127FAEB: addss       xmm6,xmm0
  000000014127FAEF: mov         rcx,qword ptr [rsp+78h]
  000000014127FAF4: mov         rbx,qword ptr [rcx+0F8h]
  000000014127FAFB: mov         rax,qword ptr [rcx+100h]
  000000014127FB02: lea         r13,[rbx+rax*4]
  000000014127FB06: cmp         byte ptr [rcx+108h],0
  000000014127FB0D: je          000000014127FBD7
  000000014127FB13: xorps       xmm7,xmm7
  000000014127FB16: xor         r14d,r14d
  000000014127FB19: movss       xmm8,dword ptr [__real@bf4a3d71]
  000000014127FB22: xor         esi,esi
  000000014127FB24: xor         r15d,r15d
  000000014127FB27: cmp         r15,rsi
  000000014127FB2A: jne         000000014127FB94
  000000014127FB2C: nop         dword ptr [rax]
  000000014127FB30: cmp         rbx,r13
  000000014127FB33: je          000000014127FCC5
  000000014127FB39: mov         ecx,dword ptr [rbx]
  000000014127FB3B: add         rbx,4
  000000014127FB3F: mov         rax,qword ptr [r12+1A8h]
  000000014127FB47: lea         rcx,[rcx+rcx*8]
  000000014127FB4B: mov         rsi,qword ptr [rax+rcx*8+10h]
  000000014127FB50: test        rsi,rsi
  000000014127FB53: je          000000014127FB30
  000000014127FB55: lea         rax,[rax+rcx*8]
  000000014127FB59: mov         rcx,qword ptr [rsp+78h]
  000000014127FB5E: mov         rcx,qword ptr [rcx+110h]
  000000014127FB65: cmp         qword ptr [rax+40h],rcx
  000000014127FB69: jbe         000000014127FB8E
  000000014127FB6B: mov         rdx,qword ptr [rax+38h]
  000000014127FB6F: mov         rcx,qword ptr [rdx+rcx*8]
  000000014127FB73: test        rcx,rcx
  000000014127FB76: je          000000014127FB8E
  000000014127FB78: mov         rax,qword ptr [rax+18h]
  000000014127FB7C: not         rcx
  000000014127FB7F: lea         rcx,[rcx+rcx*2]
  000000014127FB83: shl         rcx,4
  000000014127FB87: mov         r14,qword ptr [rax+rcx+10h]
  000000014127FB8C: jmp         000000014127FB91
  000000014127FB8E: xor         r14d,r14d
  000000014127FB91: xor         r15d,r15d
  000000014127FB94: mov         eax,r15d
  000000014127FB97: imul        rdi,rax,58h
  000000014127FB9B: add         rdi,r14
  000000014127FB9E: inc         r15
  000000014127FBA1: mov         rcx,rdi
  000000014127FBA4: call        _ZN12country_core9resources5roofs10roof_shape4Roof7is_flat17hf1ba4121fb5a18d9E
  000000014127FBA9: test        al,al
  000000014127FBAB: je          000000014127FB27
  000000014127FBB1: mov         rax,qword ptr [rsp+1E0h]
  000000014127FBB9: cmp         qword ptr [rdi],rax
  000000014127FBBC: jne         000000014127FB27
  000000014127FBC2: movaps      xmm7,xmm8
  000000014127FBC6: jmp         000000014127FB27
  000000014127FBCB: lea         rcx,[anon.d96feff8cfeff604933f686f8509a965.5.llvm.12921390545346410854]
  000000014127FBD2: call        _ZN4core6option13unwrap_failed17h837cefa1ab459360E
  000000014127FBD7: mov         qword ptr [rsp+80h],rbp
  000000014127FBDF: xorps       xmm7,xmm7
  000000014127FBE2: mov         r14d,8
  000000014127FBE8: xor         ebp,ebp
  000000014127FBEA: xor         esi,esi
  000000014127FBEC: xor         r15d,r15d
  000000014127FBEF: cmp         r15,rsi
  000000014127FBF2: jne         000000014127FC81
  000000014127FBF8: mov         rbp,qword ptr [rsp+80h]
  000000014127FC00: cmp         rbx,r13
  000000014127FC03: je          000000014127FCC5
  000000014127FC09: mov         ecx,dword ptr [rbx]
  000000014127FC0B: add         rbx,4
  000000014127FC0F: mov         rax,qword ptr [r12+100h]
  000000014127FC17: lea         rcx,[rcx+rcx*4]
  000000014127FC1B: shl         rcx,5
  000000014127FC1F: mov         rsi,qword ptr [rax+rcx+58h]
  000000014127FC24: test        rsi,rsi
  000000014127FC27: je          000000014127FC00
  000000014127FC29: add         rax,rcx
  000000014127FC2C: mov         ecx,dword ptr [rax+94h]
  000000014127FC32: mov         rdx,qword ptr [r12+1A8h]
  000000014127FC3A: lea         r8,[rcx+rcx*8]
  000000014127FC3E: mov         rcx,qword ptr [rsp+78h]
  000000014127FC43: mov         rcx,qword ptr [rcx+110h]
  000000014127FC4A: cmp         qword ptr [rdx+r8*8+40h],rcx
  000000014127FC4F: jbe         000000014127FC78
  000000014127FC51: lea         rdx,[rdx+r8*8]
  000000014127FC55: mov         r8,qword ptr [rdx+38h]
  000000014127FC59: mov         rcx,qword ptr [r8+rcx*8]
  000000014127FC5D: test        rcx,rcx
  000000014127FC60: je          000000014127FC78
  000000014127FC62: mov         rdx,qword ptr [rdx+18h]
  000000014127FC66: not         rcx
  000000014127FC69: lea         rcx,[rcx+rcx*2]
  000000014127FC6D: shl         rcx,4
  000000014127FC71: mov         rbp,qword ptr [rdx+rcx+10h]
  000000014127FC76: jmp         000000014127FC7A
  000000014127FC78: xor         ebp,ebp
  000000014127FC7A: mov         r14,qword ptr [rax+50h]
  000000014127FC7E: xor         r15d,r15d
  000000014127FC81: mov         rax,r15
  000000014127FC84: shl         rax,4
  000000014127FC88: mov         eax,dword ptr [r14+rax+8]
  000000014127FC8D: imul        rdi,rax,58h
  000000014127FC91: add         rdi,rbp
  000000014127FC94: inc         r15
  000000014127FC97: mov         rcx,rdi
  000000014127FC9A: call        _ZN12country_core9resources5roofs10roof_shape4Roof7is_flat17hf1ba4121fb5a18d9E
  000000014127FC9F: test        al,al
  000000014127FCA1: je          000000014127FBEF
  000000014127FCA7: mov         rax,qword ptr [rsp+1E0h]
  000000014127FCAF: cmp         qword ptr [rdi],rax
  000000014127FCB2: jne         000000014127FBEF
  000000014127FCB8: movss       xmm7,dword ptr [__real@bf4a3d71]
  000000014127FCC0: jmp         000000014127FBEF
  000000014127FCC5: addss       xmm6,xmm7
  000000014127FCC9: addss       xmm13,dword ptr [rsp+34h]
  000000014127FCD0: mov         al,4
  000000014127FCD2: ucomiss     xmm13,xmm6
  000000014127FCD6: mov         rcx,qword ptr [rsp+38h]
  000000014127FCDB: mov         ebx,dword ptr [rsp+30h]
  000000014127FCDF: jae         000000014127FCEC
  000000014127FCE1: mov         qword ptr [rcx],rbp
  000000014127FCE4: movss       dword ptr [rcx+8],xmm11
  000000014127FCEA: mov         eax,ebx
  000000014127FCEC: mov         byte ptr [rcx+0Ch],al
  000000014127FCEF: mov         rax,rcx
  000000014127FCF2: movaps      xmm6,xmmword ptr [rsp+0B0h]
  000000014127FCFA: movaps      xmm7,xmmword ptr [rsp+0C0h]
  000000014127FD02: movaps      xmm8,xmmword ptr [rsp+0D0h]
  000000014127FD0B: movaps      xmm9,xmmword ptr [rsp+0E0h]
  000000014127FD14: movaps      xmm10,xmmword ptr [rsp+0F0h]
  000000014127FD1D: movaps      xmm11,xmmword ptr [rsp+100h]
  000000014127FD26: movaps      xmm12,xmmword ptr [rsp+110h]
  000000014127FD2F: movaps      xmm13,xmmword ptr [rsp+120h]
  000000014127FD38: movaps      xmm14,xmmword ptr [rsp+130h]
  000000014127FD41: movaps      xmm15,xmmword ptr [rsp+140h]
  000000014127FD4A: add         rsp,158h
  000000014127FD51: pop         rbx
  000000014127FD52: pop         rbp
  000000014127FD53: pop         rdi
  000000014127FD54: pop         rsi
  000000014127FD55: pop         r12
  000000014127FD57: pop         r13
  000000014127FD59: pop         r14
  000000014127FD5B: pop         r15
  000000014127FD5D: ret
  000000014127FD5E: lea         rcx,[anon.d96feff8cfeff604933f686f8509a965.3.llvm.12921390545346410854]
  000000014127FD65: lea         r8,[anon.d96feff8cfeff604933f686f8509a965.4.llvm.12921390545346410854]
  000000014127FD6C: mov         edx,2Dh
  000000014127FD71: call        _ZN4core9panicking5panic17hb210ae7b817a9f13E
  000000014127FD76: lea         r8,[142C246E8h]
  000000014127FD7D: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014127FD82: lea         r8,[142C246E8h]
  000000014127FD89: mov         rcx,rax
  000000014127FD8C: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014127FD91: lea         r8,[142C24700h]
  000000014127FD98: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014127FD9D: lea         r8,[142C24700h]
  000000014127FDA4: mov         rcx,rax
  000000014127FDA7: call        _ZN4core9panicking18panic_bounds_check17h213dd6812833a70bE
  000000014127FDAC: int         3

; ====================================================================================================
; calculate_decorator_interaction_intent：调用 snap_balcony_door(dl=0) 并按 doorish 采纳结果
; ---- _ZN16system_decorator10ui_systems28decorator_interaction_intent38calculate_decorator_interaction_intent17h39363d3822cf003aE:  [14127E721 .. 14127E8A6]  (84 lines)
  000000014127E721: mov         qword ptr [rsp+70h],rsi
  000000014127E726: mov         qword ptr [rsp+58h],r14
  000000014127E72B: mov         rax,qword ptr [rbp+148h]
  000000014127E732: mov         qword ptr [rsp+50h],rax
  000000014127E737: mov         qword ptr [rsp+48h],r15
  000000014127E73C: mov         r15,rbx
  000000014127E73F: mov         qword ptr [rsp+40h],rbx
  000000014127E744: movss       dword ptr [rsp+30h],xmm11
  000000014127E74B: movss       dword ptr [rsp+28h],xmm0
  000000014127E751: movss       dword ptr [rsp+20h],xmm1
  000000014127E757: mov         byte ptr [rsp+60h],0
  000000014127E75C: xor         esi,esi
  000000014127E75E: lea         rcx,[rbp+0F0h]
  000000014127E765: xor         edx,edx
  000000014127E767: mov         r14,qword ptr [rbp+2B0h]
  000000014127E76E: mov         r8,r14
  000000014127E771: mov         r9,rdi
  000000014127E774: call        _ZN16system_decorator10ui_systems28decorator_interaction_intent17snap_balcony_door17hc584b6d3d4613812E
  000000014127E779: movzx       ebx,byte ptr [rbp+0FCh]
  000000014127E780: lea         eax,[rbx-3]
  000000014127E783: cmp         al,2
  000000014127E785: jae         000000014127E792
  000000014127E787: xor         edi,edi
  000000014127E789: mov         r14,qword ptr [rbp+140h]
  000000014127E790: jmp         000000014127E7F7
  000000014127E792: mov         rsi,qword ptr [rbp+0F0h]
  000000014127E799: movd        xmm6,dword ptr [rbp+0F8h]
  000000014127E7A1: mov         rcx,r14
  000000014127E7A4: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo15try_dst_as_wall17h8f2fb6f6186ab9edE
  000000014127E7A9: test        rax,rax
  000000014127E7AC: je          000000014127E7C5
  000000014127E7AE: mov         rcx,rax
  000000014127E7B1: add         rcx,8
  000000014127E7B5: mov         r14,rax
  000000014127E7B8: call        _ZN12country_core9resources5walls6WallId8is_valid17ha6e55edfa838d994E
  000000014127E7BD: mov         edi,eax
  000000014127E7BF: or          dil,byte ptr [r14+21h]
  000000014127E7C3: jmp         000000014127E7C7
  000000014127E7C5: xor         edi,edi
  000000014127E7C7: or          dil,byte ptr [rbp+2E8h]
  000000014127E7CE: and         dil,1
  000000014127E7D2: cmp         bl,1
  000000014127E7D5: sete        al
  000000014127E7D8: test        dil,dil
  000000014127E7DB: mov         r14,qword ptr [rbp+140h]
  000000014127E7E2: cmovne      r14,rsi
  000000014127E7E6: jne         000000014127E7ED
  000000014127E7E8: movdqa      xmm6,xmm9
  000000014127E7ED: and         dil,al
  000000014127E7F0: xor         esi,esi
  000000014127E7F2: movdqa      xmm9,xmm6
  000000014127E7F7: mov         eax,dword ptr [rbp+0D0h]
  000000014127E7FD: mov         dword ptr [rbp+0F0h],eax
  000000014127E803: movss       dword ptr [rbp+0F4h],xmm8
  000000014127E80C: pxor        xmm1,xmm1
  000000014127E810: addss       xmm1,xmm15
  000000014127E815: lea         rcx,[rbp+0F0h]
  000000014127E81C: call        _ZN91_$LT$utils..wall_space..WallSpace$u20$as$u20$utils..comparative_space..ComparativeSpace$GT$7project17hb54ceb2dc35b6a0dE
  000000014127E821: cmp         eax,1
  000000014127E824: jne         000000014127E8CF
  000000014127E82A: movzx       eax,byte ptr [rbp+54h]
  000000014127E82E: mov         byte ptr [r12+3Fh],al
  000000014127E833: mov         eax,dword ptr [rbp+50h]
  000000014127E836: mov         dword ptr [r12+3Bh],eax
  000000014127E83B: mov         eax,dword ptr [rbp+58h]
  000000014127E83E: mov         rdx,qword ptr [rbp+178h]
  000000014127E845: mov         rcx,qword ptr [rdx]
  000000014127E848: mov         qword ptr [r12+50h],rcx
  000000014127E84D: mov         ecx,dword ptr [rdx+8]
  000000014127E850: mov         dword ptr [r12+58h],ecx
  000000014127E855: mov         rcx,qword ptr [rbp+2E0h]
  000000014127E85C: mov         qword ptr [r12+8],rcx
  000000014127E861: mov         qword ptr [r12+10h],0
  000000014127E86A: mov         qword ptr [r12+18h],r15
  000000014127E86F: mov         dword ptr [r12+20h],1
  000000014127E878: movss       dword ptr [r12+28h],xmm15
  000000014127E87F: movd        dword ptr [r12+2Ch],xmm9
  000000014127E886: mov         qword ptr [r12+30h],r14
  000000014127E88B: movzx       ecx,byte ptr [rbp+174h]
  000000014127E892: mov         byte ptr [r12+38h],cl
  000000014127E897: mov         byte ptr [r12+39h],dil
  000000014127E89C: mov         byte ptr [r12+3Ah],r13b
  000000014127E8A1: mov         dword ptr [r12+40h],esi
  000000014127E8A6: mov         rsi,r12

; ====================================================================================================
; ui_move_decorator / ui_place_decorator：arg14 = !ActiveInputMode::is_controller()
; ---- _ZN16system_decorator10ui_systems17ui_move_decorator17ui_move_decorator17hfdf72df7da4e2d7bE:  [1407A6B80 .. 1407A6BC0]  (12 lines)
  00000001407A6B80: call        _ZN12country_core5input15ActiveInputMode13is_controller17h4819f1af199317aaE
  00000001407A6B85: mov         rcx,qword ptr [rbp+930h]
  00000001407A6B8C: xor         al,1
  00000001407A6B8E: mov         rcx,qword ptr [rcx]
  00000001407A6B91: movaps      xmm0,xmmword ptr [rbp+760h]
  00000001407A6B98: mov         qword ptr [rsp+88h],rcx
  00000001407A6BA0: mov         rcx,qword ptr [rbp+798h]
  00000001407A6BA7: mov         qword ptr [rsp+80h],rcx
  00000001407A6BAF: mov         byte ptr [rsp+78h],sil
  00000001407A6BB4: mov         rcx,qword ptr [rbp+4C8h]
  00000001407A6BBB: mov         qword ptr [rsp+70h],rcx
  00000001407A6BC0: mov         byte ptr [rsp+68h],al
; ---- _ZN16system_decorator10ui_systems18ui_place_decorator18ui_place_decorator17h616d0eb411882660E:  [1407AC44C .. 1407AC491]  (13 lines)
  00000001407AC44C: call        _ZN12country_core5input15ActiveInputMode13is_controller17h4819f1af199317aaE
  00000001407AC451: xor         al,1
  00000001407AC453: mov         rcx,qword ptr [rbp+300h]
  00000001407AC45A: mov         rdx,qword ptr [rbp+828h]
  00000001407AC461: mov         rdx,qword ptr [rdx]
  00000001407AC464: mov         byte ptr [rbp+6F7h],0
  00000001407AC46B: mov         qword ptr [rsp+88h],rdx
  00000001407AC473: mov         rdx,qword ptr [rbp+4A8h]
  00000001407AC47A: mov         qword ptr [rsp+80h],rdx
  00000001407AC482: mov         edx,dword ptr [rbp+6E0h]
  00000001407AC488: mov         byte ptr [rsp+78h],dl
  00000001407AC48C: mov         qword ptr [rsp+70h],rcx
  00000001407AC491: mov         byte ptr [rsp+68h],al

; ====================================================================================================
; PublicWallState::flat_roof_y = max_y - 0.79；Roof::is_flat = pitch <= 0.1
; ---- _ZN12country_core9resources5walls17public_wall_state15PublicWallState11flat_roof_y17he6f49c9b65aa2c97E:  [140B2A6E0 .. 140B2A701]  (7 lines)
  0000000140B2A6E0: sub         rsp,58h
  0000000140B2A6E4: test        byte ptr [rcx+108h],1
  0000000140B2A6EB: je          0000000140B2A702
  0000000140B2A6ED: movss       xmm0,dword ptr [rcx+10Ch]
  0000000140B2A6F5: addss       xmm0,dword ptr [__real@bf4a3d71]
  0000000140B2A6FD: add         rsp,58h
  0000000140B2A701: ret
; ---- _ZN12country_core9resources5roofs10roof_shape4Roof7is_flat17hf1ba4121fb5a18d9E:  [1408E3380 .. 1408E338F]  (4 lines)
  00000001408E3380: movss       xmm0,dword ptr [__real@3dcccccd]
  00000001408E3388: ucomiss     xmm0,dword ptr [rcx+30h]
  00000001408E338C: setae       al
  00000001408E338F: ret

; ====================================================================================================
; Rectangle2d::contains_point_padding：半宽 = (size + pad) * 0.5；Circle2d::signed_distance = |p-c| - r
; ---- _ZN5utils8geometry9rectangle11Rectangle2d22contains_point_padding17h6b03e3deb41d2716E:  [140C9A6A0 .. 140C9A721]  (34 lines)
  0000000140C9A6A0: sub         rsp,28h
  0000000140C9A6A4: movaps      xmmword ptr [rsp+10h],xmm7
  0000000140C9A6A9: movaps      xmmword ptr [rsp],xmm6
  0000000140C9A6AD: movsd       xmm4,mmword ptr [rcx]
  0000000140C9A6B1: movsd       xmm5,mmword ptr [rcx+8]
  0000000140C9A6B6: movshdup    xmm6,xmm4
  0000000140C9A6BA: movaps      xmm0,xmmword ptr [__xmm@80000000800000008000000080000000]
  0000000140C9A6C1: xorps       xmm6,xmm0
  0000000140C9A6C4: movaps      xmm7,xmm4
  0000000140C9A6C7: unpcklps    xmm7,xmm6
  0000000140C9A6CA: shufps      xmm7,xmm4,14h
  0000000140C9A6CE: shufps      xmm1,xmm2,0
  0000000140C9A6D2: unpcklps    xmm5,xmm5
  0000000140C9A6D5: subps       xmm1,xmm5
  0000000140C9A6D8: mulps       xmm1,xmm7
  0000000140C9A6DB: movaps      xmm2,xmm1
  0000000140C9A6DE: unpckhpd    xmm2,xmm1
  0000000140C9A6E2: movss       xmm4,dword ptr [rsp+50h]
  0000000140C9A6E8: unpcklps    xmm3,xmm4
  0000000140C9A6EB: movddup     xmm3,xmm3
  0000000140C9A6EF: movddup     xmm4,mmword ptr [rcx+10h]
  0000000140C9A6F4: addps       xmm4,xmm3
  0000000140C9A6F7: mulps       xmm4,xmmword ptr [__xmm@3f0000003f0000003f0000003f000000]
  0000000140C9A6FE: addps       xmm2,xmm1
  0000000140C9A701: xorps       xmm0,xmm2
  0000000140C9A704: movlhps     xmm0,xmm2
  0000000140C9A707: cmpleps     xmm0,xmm4
  0000000140C9A70B: movmskps    eax,xmm0
  0000000140C9A70E: cmp         eax,0Fh
  0000000140C9A711: sete        al
  0000000140C9A714: movaps      xmm6,xmmword ptr [rsp]
  0000000140C9A718: movaps      xmm7,xmmword ptr [rsp+10h]
  0000000140C9A71D: add         rsp,28h
  0000000140C9A721: ret
; ---- _ZN5utils8geometry6circle8Circle2d15signed_distance17h70ddda9f493d9e93E:  [140C98120 .. 140C9813E]  (9 lines)
  0000000140C98120: movsd       xmm0,mmword ptr [rcx]
  0000000140C98124: unpcklps    xmm1,xmm2
  0000000140C98127: subps       xmm1,xmm0
  0000000140C9812A: mulps       xmm1,xmm1
  0000000140C9812D: movshdup    xmm0,xmm1
  0000000140C98131: addss       xmm0,xmm1
  0000000140C98135: sqrtss      xmm0,xmm0
  0000000140C98139: subss       xmm0,dword ptr [rcx+8]
  0000000140C9813E: ret

; ====================================================================================================
; PublicWallState+0x280 = 墙底抬升 y（立柱判据使用）
; ---- _ZN23system_wall_constructor28construct_elevation_supports17rectangle_pillars27construct_rectangle_pillars17hed387fd6b5d8ab58E:  [14124FE06 .. 14124FE37]  (10 lines)
  000000014124FE06: mov         rax,qword ptr [rbp+0B0h]
  000000014124FE0D: movss       xmm6,dword ptr [rax+280h]
  000000014124FE15: mov         byte ptr [rbp+0C7h],1
  000000014124FE1C: mov         rcx,qword ptr [rbp+208h]
  000000014124FE23: movaps      xmm1,xmm0
  000000014124FE26: call        _ZN12country_core9resources14terrain_height28TerrainHeightsData$LT$Ty$GT$18sample_at_world_xz17h51d12d20427988f4E
  000000014124FE2B: lea         rax,[rsi+1]
  000000014124FE2F: addss       xmm0,xmm13
  000000014124FE34: ucomiss     xmm0,xmm6
  000000014124FE37: ja          000000014124FDD0

; ====================================================================================================
; convert_raycast_hit_to_decorator_dst：命中种类跳表 0x142C24118，0x0D -> StairAttachment（snap 射线只查它）
; ---- _ZN16system_decorator10ui_systems23calculate_decorator_dst36convert_raycast_hit_to_decorator_dst17h0b0a50c2d606a4aaE:  [14127C99C .. 14127C9F3]  (28 lines)
  000000014127C99C: movzx       eax,byte ptr [r9]
  000000014127C9A0: lea         rdx,[142C24118h]
  000000014127C9A7: movsxd      rax,dword ptr [rdx+rax*4]
  000000014127C9AB: add         rax,rdx
  000000014127C9AE: jmp         rax
  000000014127C9B0: mov         byte ptr [rcx],4
  000000014127C9B3: mov         rax,rcx
  000000014127C9B6: movaps      xmm6,xmmword ptr [rsp+60h]
  000000014127C9BB: add         rsp,70h
  000000014127C9BF: pop         rbx
  000000014127C9C0: pop         rdi
  000000014127C9C1: pop         rsi
  000000014127C9C2: ret
  000000014127C9C3: mov         rax,qword ptr [r9+8]
  000000014127C9C7: mov         byte ptr [rcx],0
  000000014127C9CA: mov         qword ptr [rcx+8],rax
  000000014127C9CE: jmp         000000014127C9B3
  000000014127C9D0: mov         rsi,rcx
  000000014127C9D3: movsd       xmm6,mmword ptr [r9+4]
  000000014127C9D9: add         r8,46h
  000000014127C9DD: mov         rcx,r8
  000000014127C9E0: call        _ZN12country_core9resources5walls9decorator13DecoratorType22can_place_on_stair_top17hac9011691cfee1c1E
  000000014127C9E5: test        al,al
  000000014127C9E7: je          000000014127CA10
  000000014127C9E9: mov         rcx,rsi
  000000014127C9EC: mov         byte ptr [rsi],3
  000000014127C9EF: movlps      qword ptr [rsi+4],xmm6
  000000014127C9F3: jmp         000000014127C9B3

; ====================================================================================================
; try_get_grab_offset_ss_if_valid：墙挂件的抓取参考点 = coord(u,y) 世界点 + 朝外 0.315 m
; ---- _ZN16system_decorator10ui_systems31calculate_decorator_grab_offset31try_get_grab_offset_ss_if_valid17hcf12548083e8d8f6E:  [141277F18 .. 141277FC2]  (44 lines)
  0000000141277F18: lea         rcx,[rsp+48h]
  0000000141277F1D: lea         rdx,[rsp+0A8h]
  0000000141277F25: mov         r8,r14
  0000000141277F28: call        _ZN5utils10wall_space14WallSpaceCoord14to_world_space17h83d891612441f5e5E
  0000000141277F2D: movaps      xmm0,xmmword ptr [rbp]
  0000000141277F31: movaps      xmm2,xmm0
  0000000141277F34: mulps       xmm2,xmm0
  0000000141277F37: movshdup    xmm1,xmm2
  0000000141277F3B: addss       xmm1,xmm2
  0000000141277F3F: movaps      xmm3,xmm2
  0000000141277F42: unpckhpd    xmm3,xmm2
  0000000141277F46: addss       xmm3,xmm1
  0000000141277F4A: shufps      xmm3,xmm3,0
  0000000141277F4E: shufps      xmm2,xmm2,0FFh
  0000000141277F52: subps       xmm2,xmm3
  0000000141277F55: movaps      xmm1,xmmword ptr [__xmm@000000003ea147ae0000000000000000]
  0000000141277F5C: mulps       xmm2,xmm1
  0000000141277F5F: mulps       xmm1,xmm0
  0000000141277F62: movshdup    xmm3,xmm1
  0000000141277F66: addss       xmm3,xmm1
  0000000141277F6A: movhlps     xmm1,xmm1
  0000000141277F6D: addss       xmm1,xmm3
  0000000141277F71: addss       xmm1,xmm1
  0000000141277F75: shufps      xmm1,xmm1,0
  0000000141277F79: mulps       xmm1,xmm0
  0000000141277F7C: addps       xmm1,xmm2
  0000000141277F7F: movss       xmm2,dword ptr [__real@3ea147ae]
  0000000141277F87: movaps      xmm3,xmm0
  0000000141277F8A: mulps       xmm3,xmm2
  0000000141277F8D: movaps      xmm4,xmm0
  0000000141277F90: shufps      xmm4,xmm0,0C9h
  0000000141277F94: mulps       xmm4,xmm2
  0000000141277F97: shufps      xmm3,xmm3,0D2h
  0000000141277F9B: subps       xmm4,xmm3
  0000000141277F9E: addps       xmm0,xmm0
  0000000141277FA1: shufps      xmm0,xmm0,0FFh
  0000000141277FA5: mulps       xmm0,xmm4
  0000000141277FA8: addps       xmm0,xmm1
  0000000141277FAB: movsd       xmm1,mmword ptr [rsp+48h]
  0000000141277FB1: addps       xmm1,xmm0
  0000000141277FB4: movhlps     xmm0,xmm0
  0000000141277FB7: addss       xmm0,dword ptr [rsp+50h]
  0000000141277FBD: movlps      qword ptr [rsp+58h],xmm1
  0000000141277FC2: movss       dword ptr [rsp+60h],xmm0

; ====================================================================================================
; is_decorator_location_valid：子类型 4/9 复跑 snap_balcony_door(dl=1)，tag 4 -> code 6（剔除）
; ---- _ZN16system_decorator19cull_oob_decorators27is_decorator_location_valid17h0422db76c3a91590E:  [140810811 .. 14081092E]  (54 lines)
  0000000140810811: cmp         dword ptr [rbp+0F8h],4
  0000000140810818: mov         r14,r12
  000000014081081B: je          000000014081082A
  000000014081081D: cmp         dword ptr [rbp+0F8h],9
  0000000140810824: jne         000000014081092E
  000000014081082A: mov         rsi,qword ptr [rbp+2F0h]
  0000000140810831: lea         rdx,[r15+88h]
  0000000140810838: lea         rcx,[rbp+178h]
  000000014081083F: movaps      xmm3,xmm7
  0000000140810842: call        _ZN5utils5curve21Curve$LT$T$C$CSem$GT$20get_tangent_at_coord17h4e8370f47c068d61E
  0000000140810847: movss       xmm0,dword ptr [rbp+178h]
  000000014081084F: movss       xmm1,dword ptr [rbp+180h]
  0000000140810857: movaps      xmm2,xmm0
  000000014081085A: mulss       xmm2,xmm0
  000000014081085E: movaps      xmm3,xmm1
  0000000140810861: mulss       xmm3,xmm1
  0000000140810865: addss       xmm3,xmm2
  0000000140810869: xorps       xmm2,xmm2
  000000014081086C: sqrtss      xmm2,xmm3
  0000000140810870: divss       xmm13,xmm2
  0000000140810875: mulss       xmm0,xmm13
  000000014081087A: mulss       xmm13,xmm1
  000000014081087F: mov         eax,dword ptr [rbp+8Ch]
  0000000140810885: mov         dword ptr [rbp+180h],eax
  000000014081088B: mov         rax,qword ptr [rbp+84h]
  0000000140810892: mov         qword ptr [rbp+178h],rax
  0000000140810899: movss       xmm1,dword ptr [rbp+168h]
  00000001408108A1: movss       xmm2,dword ptr [rbp+16Ch]
  00000001408108A9: mov         qword ptr [rsp+70h],rsi
  00000001408108AE: mov         rax,qword ptr [rbp+2E8h]
  00000001408108B5: mov         qword ptr [rsp+68h],rax
  00000001408108BA: mov         rax,qword ptr [rbp+90h]
  00000001408108C1: mov         qword ptr [rsp+58h],rax
  00000001408108C6: mov         rax,qword ptr [rbp+2D8h]
  00000001408108CD: mov         qword ptr [rsp+50h],rax
  00000001408108D2: mov         qword ptr [rsp+48h],r15
  00000001408108D7: mov         qword ptr [rsp+40h],rbx
  00000001408108DC: movss       dword ptr [rsp+38h],xmm2
  00000001408108E2: movss       dword ptr [rsp+30h],xmm1
  00000001408108E8: movss       dword ptr [rsp+28h],xmm13
  00000001408108EF: movss       dword ptr [rsp+20h],xmm0
  00000001408108F5: mov         byte ptr [rsp+60h],1
  00000001408108FA: lea         rcx,[rbp+110h]
  0000000140810901: lea         r9,[rbp+178h]
  0000000140810908: mov         dl,1
  000000014081090A: mov         r8,r14
  000000014081090D: call        _ZN16system_decorator10ui_systems28decorator_interaction_intent17snap_balcony_door17hc584b6d3d4613812E
  0000000140810912: cmp         byte ptr [rbp+11Ch],4
  0000000140810919: jne         0000000140810927
  000000014081091B: mov         byte ptr [rdi],6
  000000014081091E: mov         qword ptr [rdi+8],rbx
  0000000140810922: jmp         00000001408106A8
  0000000140810927: mov         rsi,qword ptr [rbp+2D0h]
  000000014081092E: lea         rcx,[rbp+1A7h]

; ====================================================================================================
; should_snap_to_corner：|u - u_corner| < W/2 + 0.46 (halftimbered 0.20)
; ---- _ZN16system_decorator19cull_oob_decorators21should_snap_to_corner17h08f0383e205ea9daE:  [140811900 .. 1408119C7]  (44 lines)
  0000000140811900: push        rsi
  0000000140811901: sub         rsp,70h
  0000000140811905: movaps      xmmword ptr [rsp+60h],xmm9
  000000014081190B: movaps      xmmword ptr [rsp+50h],xmm8
  0000000140811911: movaps      xmmword ptr [rsp+40h],xmm7
  0000000140811916: movaps      xmmword ptr [rsp+30h],xmm6
  000000014081191B: movaps      xmm6,xmm2
  000000014081191E: mov         rsi,rdx
  0000000140811921: mov         byte ptr [rsp+27h],cl
  0000000140811925: lea         rcx,[rsp+27h]
  000000014081192A: call        _ZN12country_core9resources5walls9decorator13DecoratorType27should_offer_snap_to_corner17h305383b8c29d4956E
  000000014081192F: test        al,al
  0000000140811931: je          0000000140811961
  0000000140811933: movss       xmm8,dword ptr [rsp+0B0h]
  000000014081193D: movss       xmm7,dword ptr [rsp+0A0h]
  0000000140811946: lea         rcx,[rsi+299h]
  000000014081194D: call        _ZN12country_core9resources5walls16inner_wall_state9WallStyle15is_halftimbered17h83f7cb60fdfdf005E
  0000000140811952: test        al,al
  0000000140811954: jne         0000000140811965
  0000000140811956: movss       xmm9,dword ptr [__real@3eeb851f]
  000000014081195F: jmp         000000014081196E
  0000000140811961: xor         eax,eax
  0000000140811963: jmp         00000001408119AC
  0000000140811965: movss       xmm9,dword ptr [__real@3e4ccccd]
  000000014081196E: mov         rcx,rsi
  0000000140811971: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState10wall_space17h924d9f326fab1d39E
  0000000140811976: mov         dword ptr [rsp+28h],eax
  000000014081197A: movss       dword ptr [rsp+2Ch],xmm0
  0000000140811980: lea         rcx,[rsp+28h]
  0000000140811985: movaps      xmm1,xmm6
  0000000140811988: movaps      xmm2,xmm7
  000000014081198B: call        _ZN91_$LT$utils..wall_space..WallSpace$u20$as$u20$utils..comparative_space..ComparativeSpace$GT$8subtract17hf7a5d0707cc7372dE
  0000000140811990: andps       xmm0,xmmword ptr [__xmm@7fffffff7fffffff7fffffff7fffffff]
  0000000140811997: mulss       xmm8,dword ptr [__real@3f000000]
  00000001408119A0: addss       xmm8,xmm9
  00000001408119A5: ucomiss     xmm8,xmm0
  00000001408119A9: seta        al
  00000001408119AC: movaps      xmm6,xmmword ptr [rsp+30h]
  00000001408119B1: movaps      xmm7,xmmword ptr [rsp+40h]
  00000001408119B6: movaps      xmm8,xmmword ptr [rsp+50h]
  00000001408119BC: movaps      xmm9,xmmword ptr [rsp+60h]
  00000001408119C2: add         rsp,70h
  00000001408119C6: pop         rsi
  00000001408119C7: ret

; ====================================================================================================
; move_decorators_following_anchors：is_bottom_door 的门随墙变形重贴地 max(lerp,0) + H/2 + 0.05
; ---- _ZN16system_decorator33move_decorators_following_anchors33move_decorators_following_anchors17h556f3f7e77e37e7eE:  [14081D2DC .. 14081D443]  (79 lines)
  000000014081D2DC: addss       xmm6,xmm14
  000000014081D2E1: movss       dword ptr [rbx],xmm0
  000000014081D2E5: movss       dword ptr [rbx+4],xmm6
  000000014081D2EA: cmp         byte ptr [rbx+21h],1
  000000014081D2EE: jne         000000014081D1EA
  000000014081D2F4: mov         sil,byte ptr [rsi+44h]
  000000014081D2F8: lea         rcx,[rbp+10h]
  000000014081D2FC: mov         edx,r12d
  000000014081D2FF: call        _ZN12country_core9resources5walls17decorator_subtype20DerivedDecoratorInfo15try_get_subtype17hb7adecf67732b8d2E
  000000014081D304: test        rax,rax
  000000014081D307: je          000000014081D1EA
  000000014081D30D: mov         rcx,qword ptr [r13-258h]
  000000014081D314: cmp         rcx,1
  000000014081D318: je          000000014081D520
  000000014081D31E: test        rcx,rcx
  000000014081D321: je          000000014081D5E5
  000000014081D327: movss       xmm1,dword ptr [rbx]
  000000014081D32B: maxss       xmm1,xmm9
  000000014081D330: mov         rdx,qword ptr [r13-260h]
  000000014081D337: mov         qword ptr [rbp+50h],rdx
  000000014081D33B: mov         qword ptr [rbp+58h],rcx
  000000014081D33F: lea         rcx,[_ZN4core3ops8function6FnOnce9call_once17h4daf489b02095387E.llvm.12921390545346410854]
  000000014081D346: mov         qword ptr [rbp+60h],rcx
  000000014081D34A: lea         rcx,[rbp+68h]
  000000014081D34E: movups      xmmword ptr [rcx],xmm11
  000000014081D352: lea         rcx,[rbp+50h]
  000000014081D356: mov         r12,rax
  000000014081D359: call        _ZN5utils5curve11curve_slice53CurveSlice$LT$T$C$Points$C$CSem$C$RemapT$C$RemapF$GT$18extend_right_until17h53e09b3ad41649deE
  000000014081D35E: mov         ecx,dword ptr [rbp+70h]
  000000014081D361: mov         rdx,qword ptr [r13-258h]
  000000014081D368: cmp         rdx,rcx
  000000014081D36B: jbe         000000014081D64A
  000000014081D371: lea         rax,[rcx+1]
  000000014081D375: cmp         rax,rdx
  000000014081D378: jae         000000014081D658
  000000014081D37E: movss       xmm6,dword ptr [rbp+74h]
  000000014081D383: mov         rdx,qword ptr [r13-260h]
  000000014081D38A: lea         rcx,[rcx+rcx*2]
  000000014081D38E: movss       xmm7,dword ptr [rdx+rcx*4+4]
  000000014081D394: lea         rax,[rax+rax*2]
  000000014081D398: movss       xmm14,dword ptr [rdx+rax*4+4]
  000000014081D39F: movzx       ecx,byte ptr [r12]
  000000014081D3A4: mov         rdx,qword ptr [rbp+10h]
  000000014081D3A8: call        _ZN119_$LT$country_core..systems..collision..world..RaycastMeshKey$u20$as$u20$core..convert..From$LT$slotmap..KeyData$GT$$GT$4from17ha19bda4ab9b81c68E
  000000014081D3AD: mov         r12,qword ptr [rbp+18h]
  000000014081D3B1: mov         ecx,esi
  000000014081D3B3: mov         edx,eax
  000000014081D3B5: call        _ZN12country_core9resources5walls17decorator_subtype15DecoratorLibKey3new17hce67db27b83b546aE
  000000014081D3BA: mov         byte ptr [rbp+0A6h],al
  000000014081D3C0: mov         byte ptr [rbp+0A7h],dl
  000000014081D3C6: mov         rcx,r12
  000000014081D3C9: lea         rdx,[rbp+0A6h]
  000000014081D3D0: call        _ZN3std11collections4hash3map24HashMap$LT$K$C$V$C$S$GT$3get17he63405d73122ed57E.llvm.7518301806615881319
  000000014081D3D5: test        rax,rax
  000000014081D3D8: je          000000014081D669
  000000014081D3DE: subss       xmm14,xmm7
  000000014081D3E3: mulss       xmm6,xmm14
  000000014081D3E8: addss       xmm6,xmm7
  000000014081D3EC: movss       xmm0,dword ptr [rax+3Ch]
  000000014081D3F1: subss       xmm0,dword ptr [rax+38h]
  000000014081D3F6: maxss       xmm6,xmm9
  000000014081D3FB: mulss       xmm0,xmm12
  000000014081D400: addss       xmm6,xmm0
  000000014081D404: addss       xmm6,xmm13
  000000014081D409: movaps      xmm1,xmm0
  000000014081D40C: maxss       xmm1,xmm6
  000000014081D410: cmpunordss  xmm6,xmm6
  000000014081D415: movaps      xmm2,xmm6
  000000014081D418: andnps      xmm2,xmm1
  000000014081D41B: andps       xmm6,xmm0
  000000014081D41E: orps        xmm6,xmm2
  000000014081D421: movss       dword ptr [rbx+18h],xmm6
  000000014081D426: mov         rcx,qword ptr [rbp+0A8h]
  000000014081D42D: call        _ZN12country_core9resources5walls17public_wall_state15PublicWallState5max_y17hf214ae5cdebb697aE
  000000014081D432: movaps      xmm1,xmm6
  000000014081D435: subss       xmm1,xmm0
  000000014081D439: movss       dword ptr [rbx+10h],xmm1
  000000014081D43E: movss       dword ptr [rbx+4],xmm6
  000000014081D443: jmp         000000014081D1EA

; ====================================================================================================
; 窗的墙洞记录（generate_cottage_wall_windows）
; ---- _ZN16system_decorator16decorator_visual19cottage_wall_window29generate_cottage_wall_windows17h2f1bf5a114ab5a9eE:  [1407ED5C5 .. 1407ED652]  (24 lines)
  00000001407ED5C5: movss       xmm10,dword ptr [rbp+0DCh]
  00000001407ED5CE: subss       xmm10,dword ptr [rbp+0D8h]
  00000001407ED5D7: mov         rcx,qword ptr [rbp+270h]
  00000001407ED5DE: lea         rdx,[142A8A718h]
  00000001407ED5E5: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo11dst_as_wall17h1ba2d47965083fbbE
  00000001407ED5EA: movsd       xmm6,mmword ptr [rax]
  00000001407ED5EE: mov         rax,qword ptr [rbp+2A8h]
  00000001407ED5F5: mov         rsi,qword ptr [rax+0D8h]
  00000001407ED5FC: cmp         rsi,qword ptr [rax+0C8h]
  00000001407ED603: jne         00000001407ED61F
  00000001407ED605: mov         rax,qword ptr [rbp+1B8h]
  00000001407ED60C: lea         rcx,[rax+0C8h]
  00000001407ED613: lea         rdx,[142A8A730h]
  00000001407ED61A: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h46387f1473fc5240E
  00000001407ED61F: mov         rdx,qword ptr [rbp+2A8h]
  00000001407ED626: mov         rax,qword ptr [rdx+0D0h]
  00000001407ED62D: lea         rcx,[rsi+rsi*4]
  00000001407ED631: movlps      qword ptr [rax+rcx*4],xmm6
  00000001407ED635: movss       xmm0,dword ptr [rbp+290h]
  00000001407ED63D: movss       dword ptr [rax+rcx*4+8],xmm0
  00000001407ED643: movss       dword ptr [rax+rcx*4+0Ch],xmm10
  00000001407ED64A: mov         byte ptr [rax+rcx*4+10h],0
  00000001407ED64F: inc         rsi
  00000001407ED652: mov         qword ptr [rdx+0D8h],rsi

; ====================================================================================================
; 门的墙洞记录 + 门扇网格名表（generate_cottage_balcony_doors）
; ---- _ZN16system_decorator16decorator_visual20cottage_balcony_door30generate_cottage_balcony_doors17hbc6524b595605374E:  [1407F4AEA .. 1407F4BB3]  (37 lines)
  00000001407F4AEA: mov         rdx,qword ptr [rbp+170h]
  00000001407F4AF1: movzx       edx,byte ptr [rdx-4]
  00000001407F4AF5: dec         dl
  00000001407F4AF7: cmp         dl,3
  00000001407F4AFA: jae         00000001407F59E9
  00000001407F4B00: movss       xmm15,dword ptr [rcx+rax*8-10h]
  00000001407F4B07: mov         r8d,dword ptr [rcx+rax*8-0Ch]
  00000001407F4B0C: mov         dword ptr [rbp+6Ch],r8d
  00000001407F4B10: movsd       xmm8,mmword ptr [rcx+rax*8-8]
  00000001407F4B17: movzx       eax,dl
  00000001407F4B1A: lea         rcx,[142A8BC20h]
  00000001407F4B21: movsxd      rsi,dword ptr [rcx+rax*4]
  00000001407F4B25: mov         rcx,qword ptr [rbp+80h]
  00000001407F4B2C: lea         rdx,[142A8BA88h]
  00000001407F4B33: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo11dst_as_wall17h1ba2d47965083fbbE
  00000001407F4B38: movq        xmm0,mmword ptr [rax]
  00000001407F4B3C: movdqa      xmmword ptr [rbp+0B0h],xmm0
  00000001407F4B44: mov         rax,qword ptr [rbp+1C8h]
  00000001407F4B4B: mov         rdi,qword ptr [rax+0D8h]
  00000001407F4B52: cmp         rdi,qword ptr [rax+0C8h]
  00000001407F4B59: jne         00000001407F4B75
  00000001407F4B5B: mov         rax,qword ptr [rbp+1C8h]
  00000001407F4B62: lea         rcx,[rax+0C8h]
  00000001407F4B69: lea         rdx,[142A8BAA0h]
  00000001407F4B70: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h46387f1473fc5240E
  00000001407F4B75: movshdup    xmm0,xmm8
  00000001407F4B7A: subss       xmm0,xmm8
  00000001407F4B7F: mov         rdx,qword ptr [rbp+1C8h]
  00000001407F4B86: mov         rax,qword ptr [rdx+0D0h]
  00000001407F4B8D: lea         rcx,[rdi+rdi*4]
  00000001407F4B91: movdqa      xmm1,xmmword ptr [rbp+0B0h]
  00000001407F4B99: movq        mmword ptr [rax+rcx*4],xmm1
  00000001407F4B9E: movss       dword ptr [rax+rcx*4+8],xmm15
  00000001407F4BA5: movss       dword ptr [rax+rcx*4+0Ch],xmm0
  00000001407F4BAB: mov         byte ptr [rax+rcx*4+10h],0
  00000001407F4BB0: inc         rdi
  00000001407F4BB3: mov         qword ptr [rdx+0D8h],rdi

; ====================================================================================================
; gothic 门：按 rank 推三块台阶矩形洞（每条 12 字节 {dy, w, h}，单位米）
; ---- _ZN16system_decorator16decorator_visual19gothic_balcony_door29generate_gothic_balcony_doors17hda4ebdf3734f3c9cE:  [1407F16CF .. 1407F194B]  (113 lines)
  00000001407F16CF: cmp         eax,1
  00000001407F16D2: mov         qword ptr [rbp+170h],rbx
  00000001407F16D9: je          00000001407F176B
  00000001407F16DF: cmp         eax,2
  00000001407F16E2: je          00000001407F172C
  00000001407F16E4: cmp         eax,3
  00000001407F16E7: jne         00000001407F2C49
  00000001407F16ED: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001407F16F4: mov         ecx,24h
  00000001407F16F9: mov         edx,4
  00000001407F16FE: call        __rust_alloc
  00000001407F1703: test        rax,rax
  00000001407F1706: je          00000001407F2C63
  00000001407F170C: mov         dword ptr [rax],0BEA8F5C3h
  00000001407F1712: movss       xmm9,dword ptr [__real@bea8f5c3]
  00000001407F171B: movaps      xmm15,xmmword ptr [__xmm@3fcf5c293f9333334028f5c340028f5c]
  00000001407F1723: movaps      xmm0,xmmword ptr [__xmm@3eeb851f3f8000003fb5c28f3ec7ae14]
  00000001407F172A: jmp         00000001407F17A8
  00000001407F172C: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001407F1733: mov         ecx,24h
  00000001407F1738: mov         edx,4
  00000001407F173D: call        __rust_alloc
  00000001407F1742: test        rax,rax
  00000001407F1745: je          00000001407F2C74
  00000001407F174B: mov         dword ptr [rax],0BED1EB85h
  00000001407F1751: movss       xmm9,dword ptr [__real@bed1eb85]
  00000001407F175A: movaps      xmm15,xmmword ptr [__xmm@3fa7ae143f7ae14840147ae13fb33333]
  00000001407F1762: movaps      xmm0,xmmword ptr [__xmm@3ea3d70a3f4ccccd3fab851f3ef5c28f]
  00000001407F1769: jmp         00000001407F17A8
  00000001407F176B: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001407F1772: mov         ecx,24h
  00000001407F1777: mov         edx,4
  00000001407F177C: call        __rust_alloc
  00000001407F1781: test        rax,rax
  00000001407F1784: je          00000001407F2C85
  00000001407F178A: mov         dword ptr [rax],0BE4CCCCDh
  00000001407F1790: movss       xmm9,dword ptr [__real@be4ccccd]
  00000001407F1799: movaps      xmm15,xmmword ptr [__xmm@3f4f5c293f91eb854023d70a3f90a3d7]
  00000001407F17A1: movaps      xmm0,xmmword ptr [__xmm@3ecccccd3ed1eb853fab851f3ecccccd]
  00000001407F17A8: movups      xmmword ptr [rax+4],xmm15
  00000001407F17AD: mov         qword ptr [rbp+228h],rax
  00000001407F17B4: movups      xmmword ptr [rax+14h],xmm0
  00000001407F17B8: mov         rcx,qword ptr [rbp+140h]
  00000001407F17BF: lea         rdx,[142A8B580h]
  00000001407F17C6: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo11dst_as_wall17h1ba2d47965083fbbE
  00000001407F17CB: mov         r15,qword ptr [rbp+1B8h]
  00000001407F17D2: mov         rcx,qword ptr [rbp+188h]
  00000001407F17D9: lea         rbx,[rcx+0C8h]
  00000001407F17E0: addss       xmm9,dword ptr [rax+4]
  00000001407F17E6: movd        xmm11,dword ptr [rax]
  00000001407F17EB: mov         r13,qword ptr [r15+0D8h]
  00000001407F17F2: cmp         r13,qword ptr [r15+0C8h]
  00000001407F17F9: jne         00000001407F180A
  00000001407F17FB: mov         rcx,rbx
  00000001407F17FE: lea         rdx,[142A8B598h]
  00000001407F1805: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h46387f1473fc5240E
  00000001407F180A: mov         rax,qword ptr [r15+0D0h]
  00000001407F1811: lea         rcx,[r13*4]
  00000001407F1819: add         rcx,r13
  00000001407F181C: movd        dword ptr [rax+rcx*4],xmm11
  00000001407F1822: movss       dword ptr [rax+rcx*4+4],xmm9
  00000001407F1829: movss       dword ptr [rax+rcx*4+8],xmm15
  00000001407F1830: movshdup    xmm0,xmm15
  00000001407F1835: movss       dword ptr [rax+rcx*4+0Ch],xmm0
  00000001407F183B: mov         byte ptr [rax+rcx*4+10h],0
  00000001407F1840: inc         r13
  00000001407F1843: mov         qword ptr [r15+0D8h],r13
  00000001407F184A: mov         rax,qword ptr [rbp+228h]
  00000001407F1851: movss       xmm15,dword ptr [rax+0Ch]
  00000001407F1857: movsd       xmm9,mmword ptr [rax+10h]
  00000001407F185D: mov         rcx,qword ptr [rbp+140h]
  00000001407F1864: lea         rdx,[142A8B580h]
  00000001407F186B: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo11dst_as_wall17h1ba2d47965083fbbE
  00000001407F1870: addss       xmm15,dword ptr [rax+4]
  00000001407F1876: movd        xmm11,dword ptr [rax]
  00000001407F187B: mov         r13,qword ptr [r15+0D8h]
  00000001407F1882: cmp         r13,qword ptr [r15+0C8h]
  00000001407F1889: jne         00000001407F189A
  00000001407F188B: mov         rcx,rbx
  00000001407F188E: lea         rdx,[142A8B598h]
  00000001407F1895: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h46387f1473fc5240E
  00000001407F189A: mov         rax,qword ptr [r15+0D0h]
  00000001407F18A1: lea         rcx,[r13*4]
  00000001407F18A9: add         rcx,r13
  00000001407F18AC: movd        dword ptr [rax+rcx*4],xmm11
  00000001407F18B2: movss       dword ptr [rax+rcx*4+4],xmm15
  00000001407F18B9: movlps      qword ptr [rax+rcx*4+8],xmm9
  00000001407F18BF: mov         byte ptr [rax+rcx*4+10h],0
  00000001407F18C4: inc         r13
  00000001407F18C7: mov         qword ptr [r15+0D8h],r13
  00000001407F18CE: mov         rax,qword ptr [rbp+228h]
  00000001407F18D5: movss       xmm15,dword ptr [rax+18h]
  00000001407F18DB: movsd       xmm9,mmword ptr [rax+1Ch]
  00000001407F18E1: mov         rcx,qword ptr [rbp+140h]
  00000001407F18E8: lea         rdx,[142A8B580h]
  00000001407F18EF: call        _ZN12country_core9resources5walls9decorator13DecoratorInfo11dst_as_wall17h1ba2d47965083fbbE
  00000001407F18F4: addss       xmm15,dword ptr [rax+4]
  00000001407F18FA: movd        xmm11,dword ptr [rax]
  00000001407F18FF: mov         r13,qword ptr [r15+0D8h]
  00000001407F1906: cmp         r13,qword ptr [r15+0C8h]
  00000001407F190D: jne         00000001407F191E
  00000001407F190F: mov         rcx,rbx
  00000001407F1912: lea         rdx,[142A8B598h]
  00000001407F1919: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h46387f1473fc5240E
  00000001407F191E: mov         rax,qword ptr [r15+0D0h]
  00000001407F1925: lea         rcx,[r13*4]
  00000001407F192D: add         rcx,r13
  00000001407F1930: movd        dword ptr [rax+rcx*4],xmm11
  00000001407F1936: movss       dword ptr [rax+rcx*4+4],xmm15
  00000001407F193D: movlps      qword ptr [rax+rcx*4+8],xmm9
  00000001407F1943: mov         byte ptr [rax+rcx*4+10h],0
  00000001407F1948: inc         r13
  00000001407F194B: mov         qword ptr [r15+0D8h],r13

; ====================================================================================================
; 门：is_bottom_door 闸 -> check_for_door_stairs -> 栏杆判据 base_y > terrain + 0.15
; ---- _ZN16system_decorator16decorator_visual20cottage_balcony_door30generate_cottage_balcony_doors17hbc6524b595605374E:  [1407F5188 .. 1407F5234]  (31 lines)
  00000001407F5188: mov         rcx,qword ptr [rbp+80h]
  00000001407F518F: call        _ZN12country_core9resources5walls9decorator12DecoratorDst7as_wall17h05932608dfcddbfbE
  00000001407F5194: cmp         byte ptr [rax+21h],1
  00000001407F5198: mov         eax,dword ptr [rbp+1C4h]
  00000001407F519E: mov         ecx,eax
  00000001407F51A0: jne         00000001407F41CC
  00000001407F51A6: movaps      xmmword ptr [rbp+180h],xmm6
  00000001407F51AD: movlps      qword ptr [rbp+190h],xmm13
  00000001407F51B5: movss       dword ptr [rbp+198h],xmm12
  00000001407F51BE: mov         eax,dword ptr [rbp+0D8h]
  00000001407F51C4: lea         rcx,[rbp+19Ch]
  00000001407F51CB: mov         dword ptr [rcx+10h],eax
  00000001407F51CE: movups      xmm0,xmmword ptr [rbp+0C8h]
  00000001407F51D5: movups      xmmword ptr [rcx],xmm0
  00000001407F51D8: movdqa      xmm0,xmmword ptr [rbp-30h]
  00000001407F51DD: movdqa      xmmword ptr [rbp+0E0h],xmm0
  00000001407F51E5: movss       dword ptr [rbp+0F0h],xmm15
  00000001407F51EE: mov         eax,dword ptr [rbp+6Ch]
  00000001407F51F1: mov         dword ptr [rbp+0F4h],eax
  00000001407F51F7: movlps      qword ptr [rbp+0F8h],xmm8
  00000001407F51FF: mov         rax,qword ptr [rbp+1C8h]
  00000001407F5206: mov         qword ptr [rsp+20h],rax
  00000001407F520B: mov         rcx,qword ptr [rbp+58h]
  00000001407F520F: mov         rdx,qword ptr [rbp+10h]
  00000001407F5213: lea         r8,[rbp+180h]
  00000001407F521A: lea         r9,[rbp+0E0h]
  00000001407F5221: call        _ZN16system_decorator16decorator_visual19gothic_balcony_door21check_for_door_stairs17h8af19fcca2529531E
  00000001407F5226: mov         ecx,dword ptr [rbp+1C4h]
  00000001407F522C: test        al,al
  00000001407F522E: jne         00000001407F41CC
  00000001407F5234: mov         rcx,qword ptr [rbp+140h]
; ---- _ZN16system_decorator16decorator_visual20cottage_balcony_door30generate_cottage_balcony_doors17hbc6524b595605374E:  [1407F52CD .. 1407F53AC]  (44 lines)
  00000001407F52CD: movss       xmm8,dword ptr [rbp+1A4h]
  00000001407F52D6: mov         rdx,qword ptr [r8-260h]
  00000001407F52DD: lea         rcx,[rcx+rcx*2]
  00000001407F52E1: movss       xmm14,dword ptr [rdx+rcx*4+4]
  00000001407F52E8: lea         rax,[rax+rax*2]
  00000001407F52EC: movss       xmm15,dword ptr [rdx+rax*4+4]
  00000001407F52F3: movaps      xmm11,xmm10
  00000001407F52F7: subss       xmm11,xmm8
  00000001407F52FC: movss       xmm0,dword ptr [rdx+rcx*4]
  00000001407F5301: mulss       xmm0,xmm11
  00000001407F5306: movss       xmm3,dword ptr [rdx+rcx*4+8]
  00000001407F530C: mulss       xmm3,xmm11
  00000001407F5311: movss       xmm1,dword ptr [rdx+rax*4]
  00000001407F5316: mulss       xmm1,xmm8
  00000001407F531B: addss       xmm1,xmm0
  00000001407F531F: movss       xmm2,dword ptr [rdx+rax*4+8]
  00000001407F5325: mulss       xmm2,xmm8
  00000001407F532A: addss       xmm2,xmm3
  00000001407F532E: mov         rdx,qword ptr [rbp+58h]
  00000001407F5332: mov         rax,qword ptr [rdx+8]
  00000001407F5336: mov         rcx,qword ptr [rdx+10h]
  00000001407F533A: add         rcx,rcx
  00000001407F533D: mov         qword ptr [rbp+180h],rax
  00000001407F5344: mov         qword ptr [rbp+188h],rcx
  00000001407F534B: movsd       xmm0,mmword ptr [rdx+50h]
  00000001407F5350: movsd       mmword ptr [rbp+190h],xmm0
  00000001407F5358: lea         rcx,[rbp+180h]
  00000001407F535F: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  00000001407F5364: mulss       xmm11,xmm14
  00000001407F5369: mulss       xmm8,xmm15
  00000001407F536E: addss       xmm8,xmm11
  00000001407F5373: addss       xmm0,dword ptr [__real@3e19999a]
  00000001407F537B: ucomiss     xmm8,xmm0
  00000001407F537F: mov         eax,dword ptr [rbp+1C4h]
  00000001407F5385: mov         ecx,eax
  00000001407F5387: jbe         00000001407F41CC
  00000001407F538D: mov         rax,qword ptr [rbp+170h]
  00000001407F5394: movzx       eax,byte ptr [rax-4]
  00000001407F5398: dec         al
  00000001407F539A: cmp         al,3
  00000001407F539C: jae         00000001407F5A22
  00000001407F53A2: movzx       eax,al
  00000001407F53A5: lea         rcx,[142A8BC2Ch]
  00000001407F53AC: movsxd      rdi,dword ptr [rcx+rax*4]

; ====================================================================================================
; check_for_door_stairs（全函数）
; ---- _ZN16system_decorator16decorator_visual19gothic_balcony_door21check_for_door_stairs17h8af19fcca2529531E:  [1407F3340 .. 1407F3872]  (286 lines)
  00000001407F3340: push        r15
  00000001407F3342: push        r14
  00000001407F3344: push        rsi
  00000001407F3345: push        rdi
  00000001407F3346: push        rbx
  00000001407F3347: sub         rsp,120h
  00000001407F334E: movaps      xmmword ptr [rsp+110h],xmm15
  00000001407F3357: movaps      xmmword ptr [rsp+100h],xmm14
  00000001407F3360: movaps      xmmword ptr [rsp+0F0h],xmm13
  00000001407F3369: movaps      xmmword ptr [rsp+0E0h],xmm12
  00000001407F3372: movaps      xmmword ptr [rsp+0D0h],xmm11
  00000001407F337B: movaps      xmmword ptr [rsp+0C0h],xmm10
  00000001407F3384: movaps      xmmword ptr [rsp+0B0h],xmm9
  00000001407F338D: movaps      xmmword ptr [rsp+0A0h],xmm8
  00000001407F3396: movaps      xmmword ptr [rsp+90h],xmm7
  00000001407F339E: movaps      xmmword ptr [rsp+80h],xmm6
  00000001407F33A6: mov         rdi,r9
  00000001407F33A9: mov         rbx,r8
  00000001407F33AC: mov         rsi,rdx
  00000001407F33AF: movss       xmm12,dword ptr [r8+10h]
  00000001407F33B5: movss       xmm6,dword ptr [r8+14h]
  00000001407F33BB: movss       xmm13,dword ptr [r8+18h]
  00000001407F33C1: mov         r14,qword ptr [rcx+8]
  00000001407F33C5: mov         r15,qword ptr [rcx+10h]
  00000001407F33C9: add         r15,r15
  00000001407F33CC: movsd       xmm14,mmword ptr [rcx+50h]
  00000001407F33D2: mov         qword ptr [rsp+30h],r14
  00000001407F33D7: mov         qword ptr [rsp+38h],r15
  00000001407F33DC: movsd       mmword ptr [rsp+40h],xmm14
  00000001407F33E3: lea         rcx,[rsp+30h]
  00000001407F33E8: movaps      xmm1,xmm12
  00000001407F33EC: movaps      xmm2,xmm13
  00000001407F33F0: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  00000001407F33F5: movaps      xmm8,xmm0
  00000001407F33F9: xorps       xmm7,xmm7
  00000001407F33FC: maxss       xmm8,xmm7
  00000001407F3401: mov         rcx,rsi
  00000001407F3404: call        _ZN100_$LT$country_core..resources..world_raster_defs..GardenRaster$u20$as$u20$core..ops..deref..Deref$GT$5deref17h37f159664114cf7aE
  00000001407F3409: lea         rcx,[rax+8]
  00000001407F340D: lea         rdx,[rax+20h]
  00000001407F3411: lea         r8,[rax+10h]
  00000001407F3415: lea         r9,[rax+28h]
  00000001407F3419: cmp         byte ptr [rax+298h],0
  00000001407F3420: cmovne      rcx,rdx
  00000001407F3424: cmove       r9,r8
  00000001407F3428: mov         rax,qword ptr [r9]
  00000001407F342B: mov         rcx,qword ptr [rcx]
  00000001407F342E: mov         qword ptr [rsp+30h],rcx
  00000001407F3433: mov         qword ptr [rsp+38h],rax
  00000001407F3438: mov         dword ptr [rsp+20h],43020000h
  00000001407F3440: lea         rcx,[rsp+30h]
  00000001407F3445: movss       xmm3,dword ptr [__real@43020000]
  00000001407F344D: movaps      xmm1,xmm12
  00000001407F3451: movaps      xmm2,xmm13
  00000001407F3455: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h054924d541f34fecE
  00000001407F345A: ucomiss     xmm0,xmm7
  00000001407F345D: movss       xmm0,dword ptr [rdi+1Ch]
  00000001407F3462: subss       xmm0,dword ptr [rdi+18h]
  00000001407F3467: mulss       xmm0,dword ptr [__real@bf000000]
  00000001407F346F: addss       xmm0,xmm6
  00000001407F3473: jbe         00000001407F349A
  00000001407F3475: movss       xmm1,dword ptr [__real@3dcccccd]
  00000001407F347D: ucomiss     xmm1,xmm0
  00000001407F3480: setae       sil
  00000001407F3484: jae         00000001407F34AE
  00000001407F3486: addss       xmm0,dword ptr [__real@be4ccccd]
  00000001407F348E: ucomiss     xmm8,xmm0
  00000001407F3492: jbe         00000001407F37F8
  00000001407F3498: jmp         00000001407F34AE
  00000001407F349A: addss       xmm0,dword ptr [__real@be4ccccd]
  00000001407F34A2: xor         esi,esi
  00000001407F34A4: ucomiss     xmm8,xmm0
  00000001407F34A8: jbe         00000001407F37FA
  00000001407F34AE: movaps      xmm6,xmmword ptr [rbx]
  00000001407F34B1: movaps      xmm15,xmm6
  00000001407F34B5: mulps       xmm15,xmm6
  00000001407F34B9: movshdup    xmm0,xmm15
  00000001407F34BE: addss       xmm0,xmm15
  00000001407F34C3: movaps      xmm1,xmm15
  00000001407F34C7: unpckhpd    xmm1,xmm15
  00000001407F34CC: addss       xmm1,xmm0
  00000001407F34D0: shufps      xmm1,xmm1,0
  00000001407F34D4: shufps      xmm15,xmm15,0FFh
  00000001407F34D9: subps       xmm15,xmm1
  00000001407F34DD: movaps      xmm1,xmmword ptr [__xmm@000000003f87ae140000000000000000]
  00000001407F34E4: mulps       xmm1,xmm15
  00000001407F34E8: movaps      xmm0,xmmword ptr [__xmm@000000003f87ae140000000000000000]
  00000001407F34EF: mulps       xmm0,xmm6
  00000001407F34F2: movshdup    xmm2,xmm0
  00000001407F34F6: addss       xmm2,xmm0
  00000001407F34FA: movhlps     xmm0,xmm0
  00000001407F34FD: addss       xmm0,xmm2
  00000001407F3501: addss       xmm0,xmm0
  00000001407F3505: shufps      xmm0,xmm0,0
  00000001407F3509: mulps       xmm0,xmm6
  00000001407F350C: addps       xmm0,xmm1
  00000001407F350F: movaps      xmm10,xmm6
  00000001407F3513: shufps      xmm10,xmm6,0D2h
  00000001407F3518: movaps      xmm9,xmmword ptr [__xmm@000000003f87ae140000000000000000]
  00000001407F3520: mulps       xmm9,xmm10
  00000001407F3524: xorps       xmm1,xmm1
  00000001407F3527: mulps       xmm1,xmm6
  00000001407F352A: movaps      xmmword ptr [rsp+70h],xmm1
  00000001407F352F: subps       xmm9,xmm1
  00000001407F3533: shufps      xmm9,xmm9,0D6h
  00000001407F3538: movaps      xmm7,xmm6
  00000001407F353B: addps       xmm7,xmm6
  00000001407F353E: shufps      xmm7,xmm7,0FFh
  00000001407F3542: mulps       xmm9,xmm7
  00000001407F3546: addps       xmm9,xmm0
  00000001407F354A: movaps      xmm11,xmm12
  00000001407F354E: addss       xmm11,xmm9
  00000001407F3553: movhlps     xmm9,xmm9
  00000001407F3557: addss       xmm9,xmm13
  00000001407F355C: mov         qword ptr [rsp+30h],r14
  00000001407F3561: mov         qword ptr [rsp+38h],r15
  00000001407F3566: movlps      qword ptr [rsp+40h],xmm14
  00000001407F356C: lea         rcx,[rsp+30h]
  00000001407F3571: movaps      xmm1,xmm11
  00000001407F3575: movaps      xmm2,xmm9
  00000001407F3579: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  00000001407F357E: movaps      xmm1,xmm8
  00000001407F3582: subss       xmm1,xmm0
  00000001407F3586: ucomiss     xmm1,dword ptr [__real@3dcccccd]
  00000001407F358D: jbe         00000001407F37F8
  00000001407F3593: movss       dword ptr [rsp+48h],xmm13
  00000001407F359A: movss       dword ptr [rsp+4Ch],xmm12
  00000001407F35A1: movaps      xmmword ptr [rsp+60h],xmm0
  00000001407F35A6: movss       xmm1,dword ptr [rdi+10h]
  00000001407F35AB: xorps       xmm0,xmm0
  00000001407F35AE: mulss       xmm0,xmm1
  00000001407F35B2: movaps      xmmword ptr [rsp+50h],xmm1
  00000001407F35B7: unpcklps    xmm1,xmm0
  00000001407F35BA: mulps       xmm1,xmmword ptr [__xmm@00000000000000003f0000003f000000]
  00000001407F35C1: movaps      xmm12,xmm1
  00000001407F35C5: shufps      xmm12,xmm1,54h
  00000001407F35CA: movaps      xmm0,xmm15
  00000001407F35CE: mulps       xmm0,xmm12
  00000001407F35D2: movaps      xmm2,xmm6
  00000001407F35D5: mulps       xmm2,xmm12
  00000001407F35D9: movshdup    xmm3,xmm2
  00000001407F35DD: addss       xmm3,xmm2
  00000001407F35E1: movhlps     xmm2,xmm2
  00000001407F35E4: addss       xmm2,xmm3
  00000001407F35E8: addss       xmm2,xmm2
  00000001407F35EC: shufps      xmm2,xmm2,0
  00000001407F35F0: mulps       xmm2,xmm6
  00000001407F35F3: addps       xmm2,xmm0
  00000001407F35F6: shufps      xmm1,xmm1,0D0h
  00000001407F35FA: mulps       xmm12,xmm10
  00000001407F35FE: mulps       xmm1,xmm6
  00000001407F3601: subps       xmm12,xmm1
  00000001407F3605: shufps      xmm12,xmm12,0D6h
  00000001407F360A: mulps       xmm12,xmm7
  00000001407F360E: addps       xmm12,xmm2
  00000001407F3612: movaps      xmm13,xmm12
  00000001407F3616: unpckhpd    xmm13,xmm12
  00000001407F361B: movaps      xmm1,xmm11
  00000001407F361F: subss       xmm1,xmm12
  00000001407F3624: movaps      xmm2,xmm9
  00000001407F3628: subss       xmm2,xmm13
  00000001407F362D: mov         qword ptr [rsp+30h],r14
  00000001407F3632: mov         qword ptr [rsp+38h],r15
  00000001407F3637: movlps      qword ptr [rsp+40h],xmm14
  00000001407F363D: lea         rcx,[rsp+30h]
  00000001407F3642: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  00000001407F3647: movaps      xmm1,xmm14
  00000001407F364B: movaps      xmm14,xmm0
  00000001407F364F: addss       xmm12,xmm11
  00000001407F3654: addss       xmm13,xmm9
  00000001407F3659: mov         qword ptr [rsp+30h],r14
  00000001407F365E: mov         qword ptr [rsp+38h],r15
  00000001407F3663: movlps      qword ptr [rsp+40h],xmm1
  00000001407F3668: lea         rcx,[rsp+30h]
  00000001407F366D: movaps      xmm1,xmm12
  00000001407F3671: movaps      xmm2,xmm13
  00000001407F3675: call        _ZN57_$LT$T$u20$as$u20$utils..float_grid..FloatGridSampler$GT$27bilinear_sample_at_world_xz17h21c466addbd36c5bE.llvm.10815386406131266915
  00000001407F367A: movaps      xmm9,xmm14
  00000001407F367E: cmpunordss  xmm9,xmm14
  00000001407F3684: movaps      xmm1,xmm9
  00000001407F3688: andps       xmm1,xmm0
  00000001407F368B: minss       xmm0,xmm14
  00000001407F3690: andnps      xmm9,xmm0
  00000001407F3694: orps        xmm9,xmm1
  00000001407F3698: movaps      xmm2,xmmword ptr [rsp+60h]
  00000001407F369D: movaps      xmm0,xmm2
  00000001407F36A0: minss       xmm0,xmm9
  00000001407F36A5: cmpunordss  xmm9,xmm9
  00000001407F36AB: movaps      xmm1,xmm9
  00000001407F36AF: andnps      xmm1,xmm0
  00000001407F36B2: andps       xmm9,xmm2
  00000001407F36B6: orps        xmm9,xmm1
  00000001407F36BA: movaps      xmm0,xmm8
  00000001407F36BE: subss       xmm0,xmm9
  00000001407F36C3: movss       xmm1,dword ptr [__real@3fc00000]
  00000001407F36CB: ucomiss     xmm1,xmm0
  00000001407F36CE: setbe       al
  00000001407F36D1: mov         ecx,esi
  00000001407F36D3: not         cl
  00000001407F36D5: test        cl,al
  00000001407F36D7: jne         00000001407F37F8
  00000001407F36DD: movzx       eax,byte ptr [__rust_no_alloc_shim_is_unstable]
  00000001407F36E4: mov         ecx,20h
  00000001407F36E9: mov         edx,4
  00000001407F36EE: call        __rust_alloc
  00000001407F36F3: test        rax,rax
  00000001407F36F6: je          00000001407F3863
  00000001407F36FC: mov         rdi,rax
  00000001407F36FF: mov         rbx,qword ptr [rsp+170h]
  00000001407F3707: movss       xmm1,dword ptr [__real@bf4ccccd]
  00000001407F370F: movaps      xmm0,xmm1
  00000001407F3712: subss       xmm0,xmm9
  00000001407F3717: xorps       xmm2,xmm2
  00000001407F371A: maxss       xmm0,xmm2
  00000001407F371E: mulps       xmm10,xmmword ptr [__xmm@000000003f8000000000000000000000]
  00000001407F3726: subps       xmm10,xmmword ptr [rsp+70h]
  00000001407F372C: shufps      xmm10,xmm10,0D6h
  00000001407F3731: mulps       xmm7,xmm10
  00000001407F3735: mulps       xmm15,xmmword ptr [__xmm@000000003f8000000000000000000000]
  00000001407F373D: movaps      xmm3,xmmword ptr [__xmm@000000003f8000000000000000000000]
  00000001407F3744: mulps       xmm3,xmm6
  00000001407F3747: movshdup    xmm2,xmm3
  00000001407F374B: addss       xmm2,xmm3
  00000001407F374F: movhlps     xmm3,xmm3
  00000001407F3752: addss       xmm3,xmm2
  00000001407F3756: addss       xmm3,xmm3
  00000001407F375A: shufps      xmm3,xmm3,0
  00000001407F375E: mulps       xmm3,xmm6
  00000001407F3761: addps       xmm3,xmm15
  00000001407F3765: addps       xmm3,xmm7
  00000001407F3768: movaps      xmm2,xmm3
  00000001407F376B: shufps      xmm2,xmm3,0E8h
  00000001407F376F: mulps       xmm3,xmm3
  00000001407F3772: movaps      xmm4,xmm3
  00000001407F3775: unpckhpd    xmm4,xmm3
  00000001407F3779: addss       xmm4,xmm3
  00000001407F377D: sqrtss      xmm4,xmm4
  00000001407F3781: movss       xmm3,dword ptr [__real@3f800000]
  00000001407F3789: divss       xmm3,xmm4
  00000001407F378D: test        sil,sil
  00000001407F3790: movss       xmm4,dword ptr [rsp+4Ch]
  00000001407F3796: movss       xmm5,dword ptr [rsp+48h]
  00000001407F379C: jne         00000001407F37A2
  00000001407F379E: movaps      xmm1,xmm9
  00000001407F37A2: movss       dword ptr [rdi],xmm4
  00000001407F37A6: movss       dword ptr [rdi+4],xmm5
  00000001407F37AB: movsldup    xmm3,xmm3
  00000001407F37AF: mulps       xmm2,xmm3
  00000001407F37B2: movlps      qword ptr [rdi+8],xmm2
  00000001407F37B6: movss       dword ptr [rdi+10h],xmm8
  00000001407F37BC: movss       dword ptr [rdi+14h],xmm1
  00000001407F37C1: movss       dword ptr [rdi+18h],xmm0
  00000001407F37C6: movaps      xmm0,xmmword ptr [rsp+50h]
  00000001407F37CB: movss       dword ptr [rdi+1Ch],xmm0
  00000001407F37D0: mov         rcx,qword ptr [rbx+120h]
  00000001407F37D7: test        rcx,rcx
  00000001407F37DA: je          00000001407F37EC
  00000001407F37DC: mov         edx,20h
  00000001407F37E1: mov         r8d,4
  00000001407F37E7: call        __rust_dealloc
  00000001407F37EC: mov         qword ptr [rbx+120h],rdi
  00000001407F37F3: mov         sil,1
  00000001407F37F6: jmp         00000001407F37FA
  00000001407F37F8: xor         esi,esi
  00000001407F37FA: mov         eax,esi
  00000001407F37FC: movaps      xmm6,xmmword ptr [rsp+80h]
  00000001407F3804: movaps      xmm7,xmmword ptr [rsp+90h]
  00000001407F380C: movaps      xmm8,xmmword ptr [rsp+0A0h]
  00000001407F3815: movaps      xmm9,xmmword ptr [rsp+0B0h]
  00000001407F381E: movaps      xmm10,xmmword ptr [rsp+0C0h]
  00000001407F3827: movaps      xmm11,xmmword ptr [rsp+0D0h]
  00000001407F3830: movaps      xmm12,xmmword ptr [rsp+0E0h]
  00000001407F3839: movaps      xmm13,xmmword ptr [rsp+0F0h]
  00000001407F3842: movaps      xmm14,xmmword ptr [rsp+100h]
  00000001407F384B: movaps      xmm15,xmmword ptr [rsp+110h]
  00000001407F3854: add         rsp,120h
  00000001407F385B: pop         rbx
  00000001407F385C: pop         rdi
  00000001407F385D: pop         rsi
  00000001407F385E: pop         r14
  00000001407F3860: pop         r15
  00000001407F3862: ret
  00000001407F3863: mov         ecx,4
  00000001407F3868: mov         edx,20h
  00000001407F386D: call        _ZN5alloc5alloc18handle_alloc_error17h3e7daf9bcd04547aE
  00000001407F3872: int         3

; ====================================================================================================
; add_door_autoclutter（全函数）：clutter_version>=4；hash&3: 0 doorbell (-0.25,0.74,0.10) / 1 krans (0,0.5,0.2)
; ---- _ZN16system_decorator16decorator_visual20cottage_balcony_door20add_door_autoclutter17h5b83109db320e62bE:  [1407F5E70 .. 1407F6184]  (183 lines)
  00000001407F5E70: push        r15
  00000001407F5E72: push        r14
  00000001407F5E74: push        r12
  00000001407F5E76: push        rsi
  00000001407F5E77: push        rdi
  00000001407F5E78: push        rbx
  00000001407F5E79: sub         rsp,78h
  00000001407F5E7D: movaps      xmmword ptr [rsp+60h],xmm8
  00000001407F5E83: movaps      xmmword ptr [rsp+50h],xmm7
  00000001407F5E88: movaps      xmmword ptr [rsp+40h],xmm6
  00000001407F5E8D: cmp         byte ptr [rcx+45h],4
  00000001407F5E91: jb          00000001407F6167
  00000001407F5E97: mov         edi,r9d
  00000001407F5E9A: mov         rsi,r8
  00000001407F5E9D: mov         rbx,rdx
  00000001407F5EA0: mov         r14,qword ptr [rsp+0D0h]
  00000001407F5EA8: mov         ecx,dword ptr [rcx+2Ch]
  00000001407F5EAB: call        _ZN5utils3rng8init_rng17h2380124e07933713E
  00000001407F5EB0: mov         rcx,0A0761D6478BD642Fh
  00000001407F5EBA: add         rcx,rax
  00000001407F5EBD: mov         rax,0E7037ED1A0B428DBh
  00000001407F5EC7: xor         rax,rcx
  00000001407F5ECA: mul         rax,rcx
  00000001407F5ECD: shrd        rax,rdx,1Eh
  00000001407F5ED2: shr         rdx,1Eh
  00000001407F5ED6: xor         rax,rdx
  00000001407F5ED9: test        al,3
  00000001407F5EDB: je          00000001407F6039
  00000001407F5EE1: and         eax,3
  00000001407F5EE4: cmp         eax,1
  00000001407F5EE7: jne         00000001407F6167
  00000001407F5EED: mov         r12,qword ptr [rsp+0D8h]
  00000001407F5EF5: movaps      xmm6,xmmword ptr [rbx]
  00000001407F5EF8: movsd       xmm7,mmword ptr [rbx+10h]
  00000001407F5EFD: lea         r15,[rbx+1Ch]
  00000001407F5F01: movaps      xmm1,xmm6
  00000001407F5F04: mulps       xmm1,xmm6
  00000001407F5F07: movshdup    xmm0,xmm1
  00000001407F5F0B: addss       xmm0,xmm1
  00000001407F5F0F: movaps      xmm2,xmm1
  00000001407F5F12: unpckhpd    xmm2,xmm1
  00000001407F5F16: addss       xmm2,xmm0
  00000001407F5F1A: shufps      xmm2,xmm2,0
  00000001407F5F1E: shufps      xmm1,xmm1,0FFh
  00000001407F5F22: subps       xmm1,xmm2
  00000001407F5F25: movaps      xmm0,xmmword ptr [__xmm@000000003e4ccccd3f00000000000000]
  00000001407F5F2C: mulps       xmm1,xmm0
  00000001407F5F2F: mulps       xmm0,xmm6
  00000001407F5F32: movshdup    xmm2,xmm0
  00000001407F5F36: addss       xmm2,xmm0
  00000001407F5F3A: movhlps     xmm0,xmm0
  00000001407F5F3D: addss       xmm0,xmm2
  00000001407F5F41: addss       xmm0,xmm0
  00000001407F5F45: shufps      xmm0,xmm0,0
  00000001407F5F49: mulps       xmm0,xmm6
  00000001407F5F4C: addps       xmm0,xmm1
  00000001407F5F4F: movaps      xmm1,xmm6
  00000001407F5F52: shufps      xmm1,xmm6,0D2h
  00000001407F5F56: mulps       xmm1,xmmword ptr [__xmm@00000000000000003e4ccccd3f000000]
  00000001407F5F5D: movaps      xmm2,xmm6
  00000001407F5F60: shufps      xmm2,xmm6,0C9h
  00000001407F5F64: mulps       xmm2,xmmword ptr [__xmm@000000003f000000000000003e4ccccd]
  00000001407F5F6B: subps       xmm2,xmm1
  00000001407F5F6E: movaps      xmm8,xmm6
  00000001407F5F72: addps       xmm8,xmm6
  00000001407F5F76: shufps      xmm8,xmm8,0FFh
  00000001407F5F7B: mulps       xmm8,xmm2
  00000001407F5F7F: addps       xmm8,xmm0
  00000001407F5F83: addps       xmm7,xmm8
  00000001407F5F87: movhlps     xmm8,xmm8
  00000001407F5F8B: addss       xmm8,dword ptr [rbx+18h]
  00000001407F5F91: mov         dword ptr [rsp+28h],edi
  00000001407F5F95: lea         rax,[142A8BC00h]
  00000001407F5F9C: mov         qword ptr [rsp+30h],rax
  00000001407F5FA1: mov         qword ptr [rsp+38h],5
  00000001407F5FAA: lea         rcx,[rsp+28h]
  00000001407F5FAF: call        _ZN5utils14calculate_hash17hf0a718a70ef43cc7E
  00000001407F5FB4: mov         rdi,rax
  00000001407F5FB7: mov         rcx,qword ptr [r12]
  00000001407F5FBB: add         rcx,58h
  00000001407F5FBF: call        _ZN12country_core7systems2ui9resources8ui_state9DragState10is_started17h299ca34c42e37cb5E
  00000001407F5FC4: mov         ebx,eax
  00000001407F5FC6: or          bl,4
  00000001407F5FC9: mov         r12,qword ptr [rsi+108h]
  00000001407F5FD0: cmp         r12,qword ptr [rsi+0F8h]
  00000001407F5FD7: jne         00000001407F5FEC
  00000001407F5FD9: lea         rcx,[rsi+0F8h]
  00000001407F5FE0: lea         rdx,[142A8BC08h]
  00000001407F5FE7: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h73e7ca745ece908aE
  00000001407F5FEC: mov         rax,qword ptr [rsi+100h]
  00000001407F5FF3: lea         rcx,[r12+r12*4]
  00000001407F5FF7: shl         rcx,4
  00000001407F5FFB: mov         byte ptr [rax+rcx],bl
  00000001407F5FFE: movaps      xmmword ptr [rax+rcx+10h],xmm6
  00000001407F6003: movlps      qword ptr [rax+rcx+20h],xmm7
  00000001407F6008: movss       dword ptr [rax+rcx+28h],xmm8
  00000001407F600F: movups      xmm0,xmmword ptr [r15]
  00000001407F6013: movups      xmmword ptr [rax+rcx+2Ch],xmm0
  00000001407F6018: mov         edx,dword ptr [r15+10h]
  00000001407F601C: mov         dword ptr [rax+rcx+3Ch],edx
  00000001407F6020: mov         qword ptr [rax+rcx+40h],rdi
  00000001407F6025: mov         qword ptr [rax+rcx+48h],r14
  00000001407F602A: inc         r12
  00000001407F602D: mov         qword ptr [rsi+108h],r12
  00000001407F6034: jmp         00000001407F6167
  00000001407F6039: movaps      xmm6,xmmword ptr [rbx]
  00000001407F603C: movsd       xmm7,mmword ptr [rbx+10h]
  00000001407F6041: lea         r15,[rbx+1Ch]
  00000001407F6045: movaps      xmm1,xmm6
  00000001407F6048: mulps       xmm1,xmm6
  00000001407F604B: movshdup    xmm0,xmm1
  00000001407F604F: addss       xmm0,xmm1
  00000001407F6053: movaps      xmm2,xmm1
  00000001407F6056: unpckhpd    xmm2,xmm1
  00000001407F605A: addss       xmm2,xmm0
  00000001407F605E: shufps      xmm2,xmm2,0
  00000001407F6062: shufps      xmm1,xmm1,0FFh
  00000001407F6066: subps       xmm1,xmm2
  00000001407F6069: movaps      xmm0,xmmword ptr [__xmm@000000003dcccccd3f3d70a4be800000]
  00000001407F6070: mulps       xmm1,xmm0
  00000001407F6073: mulps       xmm0,xmm6
  00000001407F6076: movshdup    xmm2,xmm0
  00000001407F607A: addss       xmm2,xmm0
  00000001407F607E: movhlps     xmm0,xmm0
  00000001407F6081: addss       xmm0,xmm2
  00000001407F6085: addss       xmm0,xmm0
  00000001407F6089: shufps      xmm0,xmm0,0
  00000001407F608D: mulps       xmm0,xmm6
  00000001407F6090: addps       xmm0,xmm1
  00000001407F6093: movaps      xmm1,xmm6
  00000001407F6096: shufps      xmm1,xmm6,0D2h
  00000001407F609A: mulps       xmm1,xmmword ptr [__xmm@00000000be8000003dcccccd3f3d70a4]
  00000001407F60A1: movaps      xmm2,xmm6
  00000001407F60A4: shufps      xmm2,xmm6,0C9h
  00000001407F60A8: mulps       xmm2,xmmword ptr [__xmm@000000003f3d70a4be8000003dcccccd]
  00000001407F60AF: subps       xmm2,xmm1
  00000001407F60B2: movaps      xmm8,xmm6
  00000001407F60B6: addps       xmm8,xmm6
  00000001407F60BA: shufps      xmm8,xmm8,0FFh
  00000001407F60BF: mulps       xmm8,xmm2
  00000001407F60C3: addps       xmm8,xmm0
  00000001407F60C7: addps       xmm7,xmm8
  00000001407F60CB: movhlps     xmm8,xmm8
  00000001407F60CF: addss       xmm8,dword ptr [rbx+18h]
  00000001407F60D5: mov         dword ptr [rsp+28h],edi
  00000001407F60D9: lea         rax,[142A8BBE0h]
  00000001407F60E0: mov         qword ptr [rsp+30h],rax
  00000001407F60E5: mov         qword ptr [rsp+38h],8
  00000001407F60EE: lea         rcx,[rsp+28h]
  00000001407F60F3: call        _ZN5utils14calculate_hash17hf0a718a70ef43cc7E
  00000001407F60F8: mov         rdi,rax
  00000001407F60FB: mov         rbx,qword ptr [rsi+108h]
  00000001407F6102: cmp         rbx,qword ptr [rsi+0F8h]
  00000001407F6109: jne         00000001407F611E
  00000001407F610B: lea         rcx,[rsi+0F8h]
  00000001407F6112: lea         rdx,[142A8BBE8h]
  00000001407F6119: call        _ZN5alloc7raw_vec19RawVec$LT$T$C$A$GT$8grow_one17h73e7ca745ece908aE
  00000001407F611E: mov         rax,qword ptr [rsi+100h]
  00000001407F6125: lea         rcx,[rbx+rbx*4]
  00000001407F6129: shl         rcx,4
  00000001407F612D: mov         byte ptr [rax+rcx],3
  00000001407F6131: movaps      xmmword ptr [rax+rcx+10h],xmm6
  00000001407F6136: movlps      qword ptr [rax+rcx+20h],xmm7
  00000001407F613B: movss       dword ptr [rax+rcx+28h],xmm8
  00000001407F6142: movups      xmm0,xmmword ptr [r15]
  00000001407F6146: movups      xmmword ptr [rax+rcx+2Ch],xmm0
  00000001407F614B: mov         edx,dword ptr [r15+10h]
  00000001407F614F: mov         dword ptr [rax+rcx+3Ch],edx
  00000001407F6153: mov         qword ptr [rax+rcx+40h],rdi
  00000001407F6158: mov         qword ptr [rax+rcx+48h],r14
  00000001407F615D: inc         rbx
  00000001407F6160: mov         qword ptr [rsi+108h],rbx
  00000001407F6167: movaps      xmm6,xmmword ptr [rsp+40h]
  00000001407F616C: movaps      xmm7,xmmword ptr [rsp+50h]
  00000001407F6171: movaps      xmm8,xmmword ptr [rsp+60h]
  00000001407F6177: add         rsp,78h
  00000001407F617B: pop         rbx
  00000001407F617C: pop         rdi
  00000001407F617D: pop         rsi
  00000001407F617E: pop         r12
  00000001407F6180: pop         r14
  00000001407F6182: pop         r15
  00000001407F6184: ret
