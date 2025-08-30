
# tclcompile

The `tclcompile` package implements commands to compile Tcl scripts into the
Tcl's internal byte code form. The generated files can be loaded into a Tcl
application using the companion
[tbcload](https://github.com/tcltk-depot/tbcload) package.

As basic usage example, evaluating the following in a Tcl shell
will generate a `hello.tbc` file containing the byte code for the
Tcl script in `hello.tcl`.

```
package require tclcompiler
compiler::compile hello.tcl
```

For detailed usage, limitations and other information, see the Chapter 6 of the
[TclPro User's Guide](https://www.tcl-lang.org/software/tclpro/doc/TclProUsersGuide14.pdf).
Although dated, most of the information there is still applicable.

## License and Copyright

```
Copyright (c) 1999-2000 Ajuba Solutions
Copyright (c) 2018 ActiveState Software Inc.
Released under the BSD-3 license. See LICENSE file for details.
```
