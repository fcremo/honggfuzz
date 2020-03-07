# Description

This fork of honggfuzz has two purposes:

- document usage of external mutators and postprocessors
- add support for external encoders

The code is not fully ready (I may have broken dumb fuzzing with no instrumentation, and minimization is probably broken as well).

See `ORIGINAL_README.md` for, well, the original README.

# External mutators, postprocessors, and encoders

TODO: move this to a file in `docs`?

Honggfuzz has three options which allow to call external programs for different purposes.

In all cases the external command is called with a filename referencing a file descriptor (`/dev/fd/n`) as first argument.
This file should be overwritten to provide the output of the command. 
The file descriptor currently references a memory mapped file which does not exist on the disk.

`--mutate_cmd <cmd>`: cmd is called to provide the initial testcase to be mutated.

`--pprocess_cmd <cmd>`: the option name implies cmd should perform adjustments to the fuzzed file.

`--ffmutate_cmd <cmd>`: cmd is responsible for mutating the testcase. If this option is provided, honggfuzz will disable internal mutations.

`--encode_cmd <cmd>`: cmd should encode the testcase before it is provided to the target program. See below.

**NOTE: The external programs are called in the exact order above**

I find the invocation order and the name of the options to be confusing. `mutate_cmd` should be used not to mutate inputs, but to provide initial ones.
`pprocess_cmd` is called *before* `ffmutate_cmd`, so it is not useful for many use cases, such as recomputing a checksum.

## External encoders

External encoders are intended to be used primarily in conjunction with external mutators.
This option allows to use mutators with inputs which are not directly readable by the target program (e.g. a serialized AST).
The encoded is responsible to encode the input in some form which the target program reads. 
If this testcase increases coverage, honggfuzz will keep the pre-encoded testcase and not the encoded one, allowing it to be fed to the mutator again.

