; Local binary: D:/MyProject/Tiny Glade/tiny-glade.exe
; SHA-256: cf048e27acdc266b2efc7c094dd435bacecb3f26c2afe54de4d09cd3f7036323
; Capstone x86-64 disassembly; RIP constant annotations read from PE data.
141215c34: subss xmm8, dword ptr [rbp + 0x308]
141215c3d: movaps xmm0, xmm8
141215c41: divss xmm0, dword ptr [rip + 0x1866c33] # 142a7c87c floats=(0.6899999976158142, 0.9000000357627869, 3.5999999046325684, 0.05999999865889549)
141215c49: call 0x142923e60
141215c4e: cvttss2si rax, xmm0
141215c53: mov rcx, rax
141215c56: sar rcx, 0x3f
141215c5a: movaps xmm1, xmm0
141215c5d: subss xmm1, xmm12
141215c62: cvttss2si rdx, xmm1
141215c67: and rdx, rcx
141215c6a: or rdx, rax
141215c6d: ucomiss xmm0, xmm14
141215c71: mov r15d, 0
141215c77: cmovae r15, rdx
141215c7b: ucomiss xmm0, dword ptr [rip + 0x174445e] # 14295a0e0 floats=(1.8446742974197924e+19, -1.0, -0.5, 1.9446237926627918e+31)
141215c82: cmova r15, rsi
141215c86: cmp r15, 3
141215c8a: mov eax, 2
141215c8f: cmovb r15, rax
141215c93: movss xmm2, dword ptr [rip + 0x18739cd] # 142a89668 floats=(0.20999999344348907, 0.0, 0.0, 0.0)
141215c9b: divss xmm2, xmm8
141215ca0: lea rax, [rip + 0x1a06991] # 142c1c638 floats=(96.885009765625, 1.401298464324817e-45, 7.987401246651457e-44, 0.0)
141215ca7: mov qword ptr [rsp + 0x20], rax
141215cac: lea rcx, [rbp + 0xb0]
141215cb3: mov rdx, r15
141215cb6: lea r9, [rbp + 0x1b0]
141215cbd: call 0x140c90350
141215dd6: mov rsi, qword ptr [rbp + 0x190]
141215ddd: cmp r13, qword ptr [rbp + 0x1b8]
141215de4: je 0x141216ee0
141215dea: lea rax, [r13 + 4]
141215dee: movss xmm12, dword ptr [r13]
141215df4: movaps xmm0, xmm10
141215df8: subss xmm0, xmm12
141215dfd: mulss xmm0, dword ptr [rbp + 0x308]
141215e05: mulss xmm12, dword ptr [rbp + 0x310]
141215e0e: addss xmm12, xmm0
141215e13: test byte ptr [rbp + 0x288], 1
141215e1a: je 0x141215e30
141215e1c: movaps xmm0, xmm12
141215e20: mov r13, rax
141215e23: movaps xmm12, xmm7
141215e27: jmp 0x141215e63
141215e29: nop dword ptr [rax]
141215e30: cmp rax, qword ptr [rbp + 0x1b8]
141215e37: je 0x141216ee0
141215e3d: movss xmm0, dword ptr [r13 + 4]
141215e43: add r13, 8
141215e47: movaps xmm1, xmm10
141215e4b: subss xmm1, xmm0
141215e4f: mulss xmm1, dword ptr [rbp + 0x308]
141215e57: mulss xmm0, dword ptr [rbp + 0x310]
141215e5f: addss xmm0, xmm1
141215e63: movaps xmm7, xmm0
141215e66: lea rcx, [rbp + 0x1b0]
141215e6d: call 0x140caf380
141215e72: movaps xmm13, xmm0
141215e76: movaps xmm0, xmm10
141215e7a: subss xmm0, xmm13
141215e7f: mulss xmm0, xmm15
141215e84: mulss xmm13, xmm15
141215e89: subss xmm13, xmm0
141215e8e: addss xmm13, dword ptr [rip + 0x18673d9] # 142a7d270 floats=(0.9200000166893005, 1.9446237926627918e+31, 6.380596051513976e-10, 4.420262484927662e-05)
141215e97: cmp rsi, r15
141215ebb: lea rax, [rsi + 1]
141215ebf: mov qword ptr [rbp + 0x190], rax
141215ec6: movaps xmm9, xmm7
141215eca: subss xmm9, xmm12
141215ecf: ucomiss xmm14, xmm9
141215ed3: mov al, 1
141215ed5: mov qword ptr [rbp + 0x288], rax
141215edc: jae 0x141215dd6
141215ee2: ucomiss xmm14, xmm13
141215ee6: jae 0x141215dd6
141215eec: cmp byte ptr [rbp + 0x316], 2
141215ef3: je 0x141215f0c
141215ef5: mov rcx, r14
141215ef8: call 0x140b2a620
141215efd: ucomiss xmm12, xmm0
141215f01: jb 0x141215f0c
141215f03: movss xmm13, dword ptr [rip + 0x17c7ae0] # 1429dd9ec floats=(0.5600000023841858, 0.0, 0.0, 0.0)
141215f0c: xor eax, eax
141215f0e: test sil, 1
141215f12: sete al
141215f15: lea rcx, [rip + 0x1718a54] # 14292e970 floats=(-1.0, 1.0, 0.0, 0.0)
141215f1c: movss xmm14, dword ptr [rcx + rax*4]
141215f22: movaps xmm11, xmm13
141215f26: mulss xmm11, xmm6
141215f2b: movaps xmm8, xmm11
141215f2f: addss xmm8, dword ptr [rip + 0x18c08dc] # 142ad6814 floats=(-0.2800000011920929, 3.9000000953674316, 1.9446237926627918e+31, 6.380596051513976e-10)
141215f38: addss xmm8, dword ptr [rip + 0x1a065ab] # 142c1c4ec floats=(-0.03849999979138374, -0.10000000149011612, -0.10000000149011612, 0.0)
141215f41: movaps xmm1, xmm14
141215f45: mulss xmm1, xmm8
141215f4a: mov eax, dword ptr [rbp + 0x280]
141215f50: mov dword ptr [rbp + 0x2c0], eax
141215f56: movss xmm0, dword ptr [rbp + 0x284]
141215f5e: movss dword ptr [rbp + 0x2c4], xmm0
141215f66: addss xmm1, dword ptr [rbp + 0x24c]
141215f6e: mov rcx, r12
141215f71: call 0x140c91eb0
141215f76: andps xmm14, xmmword ptr [rip + 0x170f972] # 1429258f0 floats=(-0.0, -0.0, -0.0, -0.0)
141215f7e: mulss xmm14, xmm8
141215f83: addss xmm14, dword ptr [rbp + 0x308]
141215f8c: addss xmm12, xmm7
141215f91: mulss xmm12, xmm6
141215f96: movaps xmm1, xmm14
141215f9a: movaps xmm2, xmm12
141215f9e: call 0x140c925e0
141215fa3: movaps xmm12, xmm0
141215fa7: movaps xmm14, xmm1
141215fab: movss dword ptr [rsp + 0x20], xmm9
141215fb2: lea rcx, [rbp + 0xc8]
141215fb9: movaps xmm1, xmm0
141215fbc: movaps xmm2, xmm14
141215fc0: movaps xmm3, xmm13
141215fc4: call 0x14123fff0
141216973: movaps xmm0, xmmword ptr [rbp + 0x20]
141216977: movss dword ptr [rbp + 0x250], xmm0
14121697f: movaps xmm0, xmmword ptr [rbp + 0x30]
141216983: movaps xmmword ptr [rbp + 0x1a0], xmm0
14121698a: test sil, 1
14121698e: je 0x1412169a7
141216990: movaps xmm0, xmmword ptr [rbp + 0x40]
141216994: movss dword ptr [rbp + 0x250], xmm0
14121699c: movaps xmm0, xmmword ptr [rbp + 0x50]
1412169a0: movaps xmmword ptr [rbp + 0x1a0], xmm0
141216b7a: lea rcx, [rbp - 0x20]
141216b7e: movaps xmm1, xmmword ptr [rbp + 0x1a0]
141216b85: movss xmm2, dword ptr [rbp + 0x250]
141216b8d: call 0x140c90ed0
141216b92: lea rcx, [rbp + 0x290]
141216b99: call 0x1412402e0
141216b9e: movaps xmm13, xmm0
141216ba2: lea rcx, [rbp + 0x290]
141216ba9: call 0x1412402f0
141216bae: movss xmm1, dword ptr [rbp]
141216bb3: mulss xmm1, dword ptr [rip + 0x1a05945] # 142c1c500 floats=(0.6370000243186951, 0.0, 0.0, 0.0)
141216bbb: unpcklps xmm13, xmm0
141216bbf: shufps xmm13, xmm13, 0x40
141216bc4: mulps xmm13, xmmword ptr [rbp - 0x20]
141216bc9: movaps xmmword ptr [rbp + 0x2c0], xmm13
141216bd1: shufps xmm0, xmmword ptr [rip + 0x1a05937], 0xa0 # 142c1c510 floats=(0.0, 0.0, 0.6370000243186951, 0.6370000243186951)
141216bd9: mulps xmm0, xmmword ptr [rbp - 0x10]
141216bdd: movaps xmmword ptr [rbp + 0x2d0], xmm0
141216be4: movss dword ptr [rbp + 0x2e0], xmm1
