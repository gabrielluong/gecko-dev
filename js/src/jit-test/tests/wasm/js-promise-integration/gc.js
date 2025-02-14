// Test if we can trace roots on the alternative stack.

let i = 0;
function js_import() {
  return Promise.resolve({i: ++i});
};
let wasm_js_import = new WebAssembly.Suspending(js_import);

let wasm_gc_import = new WebAssembly.Suspending(
    async () => { gc(); }
);

var ins = wasmEvalText(`(module
  (import "m" "import" (func (result externref)))
  (import "m" "gc" (func))
  (import "m" "conv" (func (param externref) (result i32)))

    (global (export "g") (mut i32) (i32.const 0))

    (func (export "test")
      (local i32)
      i32.const 5
      local.set 0
      loop
        call 0
        call 1
        call 2
        global.get 0
        i32.add
        global.set 0
        local.get 0
        i32.const 1
        i32.sub
        local.tee 0
        br_if 0
      end
    )

)`, {
  m: {
    import: wasm_js_import,
    gc: wasm_gc_import,
    conv: ({i}) => i,
  },
});


let wrapped_export = WebAssembly.promising(ins.exports.test);

let export_promise = wrapped_export();
assertEq(0, ins.exports.g.value);
assertEq(true, export_promise instanceof Promise);
export_promise.then(() =>
  assertEq(15, ins.exports.g.value)
);
