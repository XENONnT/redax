
Summary: 

Here we reintroduce the Artificial Dead time feature for when events are missed using the artificial Dead Time channel declared by fArtificialDeadtimeChannel (differs per board V1725 790 and V1730 792 and V1725_MV 791) 


What is missing? 
- N maximal consecutive faulty packets
- Add flag to have deadtime inclusion optional 
- Mongo loggin of this 


## DAQController.cc

Round Robin formating thread dispatch changed to ADC board associative to maintain per thread board associativity for determining number of missed events. 

Missed event inference functions on a per board level, depending on the last "good" header timestamp for that board. If the same board's packets are processed by seperate threads and the next "good" header timestamp for that board is on a seperate thread we would have to deal with shared memory between threads to determine the number of missed events. 

The goal here is essentially to always be able to fully close the encountered event gaps logically as we otherwise wan't to enter a DAQ error state. 


## StraxFormater.hh 

Added per board gap bookkeeping: `GapReason` enum relevant for erroring vs continuing and `GapState` struct to track the gap on a board level. 

The need for this is introduced by attempting to track cross packet gaps of events in a controlled manor, so each thread needs to hold on to all of its boards Gap Information. The state stores the last known event counter and its tick (ADC time) an anchor, if we are in a gap, some counters for diagnostics. 

## StraxFormater.cc 

Added event counter mask and max inferred deadtime per gap alongside GetFullEventTick. 
# TODO Explain GetFullEventTick better
event counter mask extracts the event count index as returned by the ADC header, `GetFullEventTicks` provides a way to count events in a robust way against rollover logic. `max_inferred_deadtime_per_gap` provides a hard cap to the number of deadtime events that can be emmited into the artificial deadtime channel. 

`ProcessDatapacket` now marks missed events with deadtime, guards against out of memory access in case of an incomplete buffer, and sets the DAQ in error state in case of unresolvable deadtime. This corresponds to the core of the changes.

We check that the remaining words is acceptable to avoid OOB access `words < event_header_words || words > remaining` treating corruption as this condition not being met, opening a gap that reaches accross packets via the per board `GapState` struct. 

In case of a missed event mid packet (previously handled by writing a log line and dumping the invalid data to disk) a gap is opened with reason missed header scan treating it as a corruption. We then advance to the next header until the next valid header is found. 

`ProcessEvent` now closes these gaps on the next valid header, counting the number of missed events and creating the Deadtime.

This produces a sum number of missed events per run for logging purposes 

## V1724.*

`UnpackEventHeader` now returns a 5 tuple including the event_counter for the event counter tracking logic as otherwise we could not infer the number of events missed between packets as timing can not be used in DPP-DAW mode.   

Alongside this it now checks that the size of the header is valid (likely not needed, but could become valid if the the offset is just enough such that the last header is split in the buffer, but it really depends on how exactly the CAEN driver is doing its job wrong). 

Soft-error plumbing to reuse the existing DAQ error state translator, code 0x4 now corresponds to a software induced "board" error that sets the DAQ in error state in case of an unrecoverable bad header (from the start of the first packet) 

The V1274_MV.* receive these changes via inheritance.

Changed the default AQMon Channel to straxen respected 799, vetoing interval is only defined in straxen for this channel as the remaining dedicated channels never saw use. In favor of simplicity this channel is used.

## V1730.*

Matching the V1724 updates alongside making the 48 bit timestamp assembly explicit. 

Changed the default AQMon Channel to straxen respected 799, vetoing interval is only defined in straxen for this channel as the remaining dedicated channels never saw use. In favor of simplicity this channel is used.