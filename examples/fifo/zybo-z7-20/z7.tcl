set_property board_part digilentinc.com:zybo-z7-20:part0:1.0 [current_project]
set_property simulator_language Verilog [current_project]
set_property source_mgmt_mode All [current_project]

source [file join [file dirname [info script]] "design_ps.tcl"]

make_wrapper -files [get_files design_ps.bd] -top
set BD [file dirname [get_files design_ps.bd]]
add_files $BD/hdl/design_ps_wrapper.v

