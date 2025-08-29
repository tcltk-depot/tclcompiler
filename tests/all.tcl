# all.tcl for tclcompiler tests

if {![info exists ::tcl::pkg::ifneeded(tclcompiler,1.9.0)]} {
    package ifneeded tclcompiler 1.9.0 {load {} tclcompiler}
}

if {"::tcltest" ni [namespace children]} {
    package require tcltest
    namespace import -force ::tcltest::*
}

set ::tcltest::testsDirectory [file normalize [file dirname [info script]]]

configure -verbose bps

runAllTests