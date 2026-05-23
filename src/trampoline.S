.global sigreturn_stub
.global sigreturn_stub_end
.align 2
sigreturn_stub:
    li a7, 11
    ecall
sigreturn_stub_end: