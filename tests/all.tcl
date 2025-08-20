# all.tcl for tclcompiler tests

if {![info exists ::tcl::pkg::ifneeded(compiler,1.9.0)]} {
    package ifneeded compiler 1.9.0 {load {} compiler}
}

if {"::tcltest" ni [namespace children]} {
    package require tcltest
    namespace import -force ::tcltest::*
}

set ::tcltest::testsDirectory [file normalize [file dirname [info script]]]

configure -verbose bps

runAllTests