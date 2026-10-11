// Orbis TLS stubs: emulated-TLS init wrappers for thread_locals that are all
// zero-initialized (x86Ptr/xTextPtr/g_xmmtypes/j8Ptr/j32Ptr), so dynamic init
// is a no-op. Hidden + weak: satisfies static references, stays out of the
// dynamic surface the native converter validates.
.text
.weak _ZTH6x86Ptr
.hidden _ZTH6x86Ptr
.type _ZTH6x86Ptr,@function
_ZTH6x86Ptr:
	ret
.weak _ZTH5j8Ptr
.hidden _ZTH5j8Ptr
.type _ZTH5j8Ptr,@function
_ZTH5j8Ptr:
	ret
.weak _ZTH6j32Ptr
.hidden _ZTH6j32Ptr
.type _ZTH6j32Ptr,@function
_ZTH6j32Ptr:
	ret
.weak _ZTH10g_xmmtypes
.hidden _ZTH10g_xmmtypes
.type _ZTH10g_xmmtypes,@function
_ZTH10g_xmmtypes:
	ret
