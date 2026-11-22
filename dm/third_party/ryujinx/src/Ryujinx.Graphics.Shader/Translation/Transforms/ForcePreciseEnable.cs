using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    class ForcePreciseEnable : ITransformPass
    {
        public static bool IsEnabled(IGpuAccessor gpuAccessor, ShaderStage stage, TargetLanguage targetLanguage, FeatureFlags usedFeatures)
        {
            return gpuAccessor.QueryHostReducedPrecision() &&
                   (stage == ShaderStage.Fragment ||
                    stage == ShaderStage.Vertex ||
                    stage == ShaderStage.TessellationEvaluation ||
                    stage == ShaderStage.Geometry);
        }

        public static LinkedListNode<INode> RunPass(TransformContext context, LinkedListNode<INode> node)
        {
            // There are some cases where a small bias is added to values to prevent division by zero.
            // When operating with reduced precision, it is possible for this bias to get rounded to 0
            // and cause a division by zero.
            // To prevent that, we force those operations to be precise even if the host wants
            // imprecise operations for performance.

            Operation operation = (Operation)node.Value;

            if (context.Stage == ShaderStage.Fragment &&
                operation.Inst == (Instruction.FP32 | Instruction.Divide) &&
                operation.GetSource(0).Type == OperandType.Constant &&
                operation.GetSource(0).AsFloat() == 1f &&
                operation.GetSource(1).AsgOp is Operation addOp &&
                addOp.Inst == (Instruction.FP32 | Instruction.Add) &&
                addOp.GetSource(1).Type == OperandType.Constant)
            {
                addOp.ForcePrecise = true;
            }

            // nxnative: the depth a fragment shader writes and the position a vertex/geometry/tessellation shader writes
            // stay exact (NoContraction) under reduced precision. Tears of the Kingdom's light classifier writes a material
            // id as D16 depth for an EQUAL test, and its terrain depth pre-pass + G-buffer pass compare EQUAL on
            // gl_Position: without NoContraction on Position's defining arithmetic, Turnip fuses FMul+FAdd into FMA
            // differently between the pre-pass VS and G-buffer VS, leaving stale G-buffer specks along diagonal triangle
            // seams (matches TOTKDroid's "invariant gl_Position; precise gl_Position;").
            if (operation.Inst == Instruction.Store &&
                operation.StorageKind == StorageKind.Output &&
                operation.SourcesCount >= 2 &&
                operation.GetSource(0).Type == OperandType.Constant &&
                (operation.GetSource(0).Value == (int)IoVariable.FragmentOutputDepth ||
                 operation.GetSource(0).Value == (int)IoVariable.Position))
            {
                MarkPrecise(operation.GetSource(operation.SourcesCount - 1));
            }

            return node;
        }

        private static void MarkPrecise(Operand value)
        {
            HashSet<INode> seen = new();
            Stack<Operand> todo = new();
            todo.Push(value);

            while (todo.Count != 0)
            {
                if (todo.Pop()?.AsgOp is not INode def || !seen.Add(def))
                {
                    continue;
                }

                if (def is Operation op)
                {
                    op.ForcePrecise = true;
                }

                for (int i = 0; i < def.SourcesCount; i++)
                {
                    todo.Push(def.GetSource(i));
                }
            }
        }
    }
}
