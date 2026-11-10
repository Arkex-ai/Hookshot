# Masked-clear resource admission

Masked color clears use a host-authored fullscreen triangle when a channel mask
prevents using the attachment-clear fast path. Their layout, temporary modules
and graphics pipeline must all exist before the clear can be recorded. Previously
creation errors could propagate undefined layout/module outputs, while a null
pipeline could remain cached indefinitely.

Only a successful layout enters shared state. Failed module outputs are neither
used nor destroyed; a failed fragment stage releases the already-created vertex
module. Both temporary modules are released after pipeline creation. A pipeline
whose owning-cache insertion fails is destroyed. Failed keys are bounded within
a present and retry on later presents, while valid hits and unrelated keys remain
available. If failure tracking cannot allocate, new keys wait until the next
present. Fragment-word allocation failure follows the same retry contract.

The existing caller's replay lock serializes this shared state. The actual masked
clear consumer already skips a null pipeline before binding or drawing. Masks,
shader selection/output locations, render-target formats, samples, depth/stencil,
scissor/viewport, the full-mask fast path and renderer API are unchanged.

`tests/test_masked_clear_admission.py` executes the complete production factory
and SPIR-V location helper, plus the actual masked-clear caller's final binding,
state invalidation, constants and draw block. The preceding target lookup, pass
setup and fast-clear selection are outside this fixture. Vulkan, targets, the
caller lock boundary and authored shader words are modeled; shader words model
the type tag/Location decoration and are not driver-executable shaders. Standard
containers use tagged throwing allocators. Owning-insertion faults are armed
after Vulkan creates the pipeline, independent of an STL's allocation order.

31 cases cover every creation stage, null/undefined layout/module error outputs,
bounded retry, ownership and scratch/failure-metadata allocation, independent
keys and valid hits, concurrency, the final consumer, float/unsigned/signed clear
variants, attachment7, sample/depth/stencil choices and cache-key changes.

Run `python -I -B tests/run_all.py masked_clear_admission --strict`, or invoke
the test with `--headers <Vulkan include folder> --cxx <compiler>`; Linux also
supports `--sanitize`. These models do not prove native OOM recovery, campaign
coverage, visual improvements or performance gains.

Pipeline outputs follow the distinct [multiple-pipeline creation contract](https://docs.vulkan.org/spec/latest/chapters/pipelines.html#pipelines-multiple):
nonnull returned pipelines are created objects, even when a batched call fails.
This path requests one pipeline. Layout/module failure outputs remain undefined.
