export const description = `
Execution Tests for matrix indexing expressions
`;

import { makeTestGroup } from '../../../../../../common/framework/test_group.js';
import { AllFeaturesMaxLimitsGPUTest } from '../../../../../gpu_test.js';
import {
  MatrixValue,
  ScalarValue,
  Type,
  abstractFloat,
  f32,
  vec,
  Value,
} from '../../../../../util/conversion.js';
import { align } from '../../../../../util/math.js';
import { Case } from '../../case.js';
import { allInputSources, basicExpressionBuilder, run } from '../../expression.js';

export const g = makeTestGroup(AllFeaturesMaxLimitsGPUTest);

g.test('concrete_float_column')
  .specURL('https://www.w3.org/TR/WGSL/#matrix-access-expr')
  .desc(`Test indexing a column vector from a concrete matrix`)
  .params(u =>
    u
      .combine('inputSource', allInputSources)
      .combine('elementType', ['f32', 'f16'] as const)
      .combine('indexType', ['i32', 'u32'] as const)
      .combine('columns', [2, 3, 4] as const)
      .combine('rows', [2, 3, 4] as const)
  )
  .fn(async t => {
    if (t.params.elementType === 'f16') {
      t.skipIfDeviceDoesNotHaveFeature('shader-f16');
    }
    const elementType = Type[t.params.elementType];
    const indexType = Type[t.params.indexType];
    const matrixType = Type.mat(t.params.columns, t.params.rows, elementType);
    const columnType = Type.vec(t.params.rows, elementType);
    const elements: ScalarValue[][] = [];
    for (let c = 0; c < t.params.columns; c++) {
      const column: ScalarValue[] = [];
      for (let r = 0; r < t.params.rows; r++) {
        column.push(elementType.create((c + 1) * 10 + (r + 1)));
      }
      elements.push(column);
    }
    const vector = new MatrixValue(elements);
    const cases: Case[] = [];
    for (let c = 0; c < t.params.columns; c++) {
      cases.push({
        input: [vector, indexType.create(c)],
        expected: vec(...elements[c]),
      });
    }

    await run(
      t,
      basicExpressionBuilder(ops => `${ops[0]}[${ops[1]}]`),
      [matrixType, indexType],
      columnType,
      t.params,
      cases
    );
  });

g.test('concrete_float_element')
  .specURL('https://www.w3.org/TR/WGSL/#matrix-access-expr')
  .desc(`Test indexing a single element from a concrete matrix`)
  .params(u =>
    u
      .combine('inputSource', allInputSources)
      .combine('elementType', ['f32', 'f16'] as const)
      .combine('indexType', ['i32', 'u32'] as const)
      .combine('columns', [2, 3, 4] as const)
      .combine('rows', [2, 3, 4] as const)
  )
  .fn(async t => {
    if (t.params.elementType === 'f16') {
      t.skipIfDeviceDoesNotHaveFeature('shader-f16');
    }
    const elementType = Type[t.params.elementType];
    const indexType = Type[t.params.indexType];
    const matrixType = Type.mat(t.params.columns, t.params.rows, elementType);
    const columnValues: ScalarValue[][] = [];
    for (let c = 0; c < t.params.columns; c++) {
      const column: ScalarValue[] = [];
      for (let r = 0; r < t.params.rows; r++) {
        column.push(elementType.create((c + 1) * 10 + (r + 1)));
      }
      columnValues.push(column);
    }
    const matrix = new MatrixValue(columnValues);
    const cases: Case[] = [];
    for (let c = 0; c < t.params.columns; c++) {
      for (let r = 0; r < t.params.rows; r++) {
        cases.push({
          input: [matrix, indexType.create(c), indexType.create(r)],
          expected: columnValues[c][r],
        });
      }
    }

    await run(
      t,
      basicExpressionBuilder(ops => `${ops[0]}[${ops[1]}][${ops[2]}]`),
      [matrixType, indexType, indexType],
      elementType,
      t.params,
      cases
    );
  });

g.test('abstract_float_column')
  .specURL('https://www.w3.org/TR/WGSL/#matrix-access-expr')
  .desc(`Test indexing a column vector from a abstract-float matrix`)
  .params(u =>
    u
      .combine('indexType', ['i32', 'u32'] as const)
      .combine('columns', [2, 3, 4] as const)
      .combine('rows', [2, 3, 4] as const)
  )
  .fn(async t => {
    const indexType = Type[t.params.indexType];
    const matrixType = Type.mat(t.params.columns, t.params.rows, Type.abstractFloat);
    const vecfColumnType = Type.vec(t.params.rows, Type.f32);
    const values: number[][] = [];
    for (let c = 0; c < t.params.columns; c++) {
      const column: number[] = [];
      for (let r = 0; r < t.params.rows; r++) {
        column.push((c + 1) * 10 + (r + 1));
      }
      values.push(column);
    }
    const matrix = new MatrixValue(
      values.map(column => column.map(v => abstractFloat(v * 0x100000000)))
    );
    const cases: Case[] = [];
    for (let c = 0; c < t.params.columns; c++) {
      cases.push({
        input: [matrix, indexType.create(c)],
        expected: vec(...values[c].map(v => f32(v))),
      });
    }

    await run(
      t,
      basicExpressionBuilder(ops => `${ops[0]}[${ops[1]}] / 0x100000000`),
      [matrixType, indexType],
      vecfColumnType,
      { inputSource: 'const' },
      cases
    );
  });

g.test('abstract_float_element')
  .specURL('https://www.w3.org/TR/WGSL/#matrix-access-expr')
  .desc(`Test indexing a single element from a abstract-float matrix`)
  .params(u =>
    u
      .combine('indexType', ['i32', 'u32'] as const)
      .combine('columns', [2, 3, 4] as const)
      .combine('rows', [2, 3, 4] as const)
  )
  .fn(async t => {
    const indexType = Type[t.params.indexType];
    const matrixType = Type.mat(t.params.columns, t.params.rows, Type.abstractFloat);
    const values: number[][] = [];
    for (let c = 0; c < t.params.columns; c++) {
      const column: number[] = [];
      for (let r = 0; r < t.params.rows; r++) {
        column.push((c + 1) * 10 + (r + 1));
      }
      values.push(column);
    }
    const matrix = new MatrixValue(
      values.map(column => column.map(v => abstractFloat(v * 0x100000000)))
    );
    const cases: Case[] = [];
    for (let c = 0; c < t.params.columns; c++) {
      for (let r = 0; r < t.params.rows; r++) {
        cases.push({
          input: [matrix, indexType.create(c), indexType.create(r)],
          expected: f32(values[c][r]),
        });
      }
    }

    await run(
      t,
      basicExpressionBuilder(ops => `${ops[0]}[${ops[1]}][${ops[2]}] / 0x100000000`),
      [matrixType, indexType, indexType],
      Type.f32,
      { inputSource: 'const' },
      cases
    );
  });

g.test('non_const_index')
  .specURL('https://www.w3.org/TR/WGSL/#matrix-access-expr')
  .desc(`Test indexing of a matrix using non-const index`)
  .params(u => u.combine('columns', [2, 3, 4] as const).combine('rows', [2, 3, 4] as const))
  .fn(t => {
    const cols = t.params.columns;
    const rows = t.params.rows;
    const values = Array.from(Array(cols * rows).keys());
    const wgsl = `
@group(0) @binding(0) var<storage, read_write> output : array<f32, ${cols * rows}>;

@compute @workgroup_size(${cols}, ${rows})
fn main(@builtin(local_invocation_id) invocation_id : vec3<u32>) {
  let m = mat${cols}x${rows}f(${values.join(', ')});
  output[invocation_id.x*${rows} + invocation_id.y] = m[invocation_id.x][invocation_id.y];
}
`;

    const pipeline = t.device.createComputePipeline({
      layout: 'auto',
      compute: {
        module: t.device.createShaderModule({ code: wgsl }),
        entryPoint: 'main',
      },
    });

    const bufferSize = (arr: Value[]) => {
      let offset = 0;
      let alignment = 0;
      for (const value of arr) {
        alignment = Math.max(alignment, value.type.alignment);
        offset = align(offset, value.type.alignment) + value.type.size;
      }
      return align(offset, alignment);
    };

    const toArray = (arr: Value[]) => {
      const array = new Uint8Array(bufferSize(arr));
      let offset = 0;
      for (const value of arr) {
        offset = align(offset, value.type.alignment);
        value.copyTo(array, offset);
        offset += value.type.size;
      }
      return array;
    };

    const expected = values.map(i => Type['f32'].create(i));

    const outputBuffer = t.createBufferTracked({
      size: bufferSize(expected),
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC,
    });

    const bindGroup = t.device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [{ binding: 0, resource: { buffer: outputBuffer } }],
    });

    const encoder = t.device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bindGroup);
    pass.dispatchWorkgroups(1);
    pass.end();
    t.queue.submit([encoder.finish()]);

    t.expectGPUBufferValuesEqual(outputBuffer, toArray(expected));
  });
