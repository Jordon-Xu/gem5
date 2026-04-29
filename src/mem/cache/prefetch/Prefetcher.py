# Copyright (c) 2012, 2014, 2019, 2022-2025 Arm Limited
# Copyright (c) 2023 The University of Edinburgh
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2005 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.citations import add_citation
from m5.objects.ClockedObject import ClockedObject
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *
from m5.objects.Tags import *
from m5.params import *
from m5.proxy import *
from m5.SimObject import *


class HWPProbeEvent:
    def __init__(self, prefetcher, obj, *listOfNames):
        self.obj = obj
        self.prefetcher = prefetcher
        self.names = listOfNames

    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbe(
                    self.obj.getCCObject(), name
                )


class BasePrefetcher(ClockedObject):
    type = "BasePrefetcher"
    abstract = True
    cxx_class = "gem5::prefetch::Base"
    cxx_header = "mem/cache/prefetch/base.hh"
    cxx_exports = [PyBindMethod("addEventProbe"), PyBindMethod("addMMU")]
    sys = Param.System(Parent.any, "System this prefetcher belongs to")

    # Get the block size from the parent (system)
    block_size = Param.Int(Parent.cache_line_size, "Block size in bytes")

    on_miss = Param.Bool(False, "Only notify prefetcher on misses")
    on_read = Param.Bool(True, "Notify prefetcher on reads")
    on_write = Param.Bool(True, "Notify prefetcher on writes")
    on_data = Param.Bool(True, "Notify prefetcher on data accesses")
    on_inst = Param.Bool(True, "Notify prefetcher on instruction accesses")
    prefetch_on_access = Param.Bool(
        False,
        "Notify the hardware prefetcher on every access (not just misses)",
    )
    prefetch_on_pf_hit = Param.Bool(
        True,
        "Notify the hardware prefetcher on hit on prefetched lines",
    )
    use_virtual_addresses = Param.Bool(
        False, "Use virtual addresses for prefetching"
    )
    page_bytes = Param.MemorySize(
        "4KiB", "Size of pages for virtual addresses"
    )

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._events = []
        self._mmus = []

    def addEvent(self, newObject):
        self._events.append(newObject)

    # Override the normal SimObject::regProbeListeners method and
    # register deferred event handlers.
    def regProbeListeners(self):
        for mmu in self._mmus:
            self.getCCObject().addMMU(mmu.getCCObject())
        for event in self._events:
            event.register()
        self.getCCObject().regProbeListeners()

    def listenFromProbe(self, simObj, *probeNames):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        if len(probeNames) <= 0:
            raise TypeError("probeNames must have at least one element")
        self.addEvent(HWPProbeEvent(self, simObj, *probeNames))

    def registerMMU(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._mmus.append(simObj)


class MultiPrefetcher(BasePrefetcher):
    type = "MultiPrefetcher"
    cxx_class = "gem5::prefetch::Multi"
    cxx_header = "mem/cache/prefetch/multi.hh"

    prefetchers = VectorParam.BasePrefetcher([], "Array of prefetchers")


class QueuedPrefetcher(BasePrefetcher):
    type = "QueuedPrefetcher"
    abstract = True
    cxx_class = "gem5::prefetch::Queued"
    cxx_header = "mem/cache/prefetch/queued.hh"
    latency = Param.Int(1, "Latency for generated prefetches")
    queue_size = Param.Int(32, "Maximum number of queued prefetches")
    max_prefetch_requests_with_pending_translation = Param.Int(
        32,
        "Maximum number of queued prefetches that have a missing translation",
    )
    queue_squash = Param.Bool(True, "Squash queued prefetch on demand access")
    queue_filter = Param.Bool(True, "Don't queue redundant prefetches")
    cache_snoop = Param.Bool(
        False, "Snoop cache to eliminate redundant request"
    )

    tag_prefetch = Param.Bool(
        True, "Tag prefetch with PC of generating access"
    )

    # The throttle_control_percentage controls how many of the candidate
    # addresses generated by the prefetcher will be finally turned into
    # prefetch requests
    # - If set to 100, all candidates can be discarded (one request
    #   will always be allowed to be generated)
    # - Setting it to 0 will disable the throttle control, so requests are
    #   created for all candidates
    # - If set to 60, 40% of candidates will generate a request, and the
    #   remaining 60% will be generated depending on the current accuracy
    throttle_control_percentage = Param.Percent(
        0,
        "Percentage of requests \
        that can be throttled depending on the accuracy of the prefetcher.",
    )


class StridePrefetcherHashedSetAssociative(TaggedSetAssociative):
    type = "StridePrefetcherHashedSetAssociative"
    cxx_class = "gem5::prefetch::StridePrefetcherHashedSetAssociative"
    cxx_header = "mem/cache/prefetch/stride.hh"


class StridePrefetcher(QueuedPrefetcher):
    type = "StridePrefetcher"
    cxx_class = "gem5::prefetch::Stride"
    cxx_header = "mem/cache/prefetch/stride.hh"

    # Do not consult stride prefetcher on instruction accesses
    on_inst = False

    confidence_counter_bits = Param.Unsigned(
        3, "Number of bits of the confidence counter"
    )
    initial_confidence = Param.Unsigned(
        4, "Starting confidence of new entries"
    )
    confidence_threshold = Param.Percent(
        50, "Prefetch generation confidence threshold"
    )

    use_requestor_id = Param.Bool(True, "Use requestor id based history")

    use_cache_line_address = Param.Bool(
        True,
        "If this parameter is set to True, then the prefetcher will "
        "operate on cache line addresses, else it would operate on word "
        "addresses",
    )

    degree = Param.Int(4, "Number of prefetches to generate")
    distance = Param.Unsigned(
        0,
        "How far ahead of the demand stream to start prefetching. "
        "Skip this number of strides ahead of the first identified prefetch, "
        "then generate `degree` prefetches at `stride` intervals. "
        "A value of zero indicates no skip.",
    )

    table_assoc = Param.Int(4, "Associativity of the PC table")
    table_entries = Param.MemorySize("64", "Number of entries of the PC table")
    table_indexing_policy = Param.TaggedIndexingPolicy(
        StridePrefetcherHashedSetAssociative(
            entry_size=1, assoc=Parent.table_assoc, size=Parent.table_entries
        ),
        "Indexing policy of the PC table",
    )
    table_replacement_policy = Param.BaseReplacementPolicy(
        RandomRP(), "Replacement policy of the PC table"
    )


class TaggedPrefetcher(QueuedPrefetcher):
    type = "TaggedPrefetcher"
    cxx_class = "gem5::prefetch::Tagged"
    cxx_header = "mem/cache/prefetch/tagged.hh"

    degree = Param.Int(2, "Number of prefetches to generate")


class IndirectMemoryPrefetcher(QueuedPrefetcher):
    type = "IndirectMemoryPrefetcher"
    cxx_class = "gem5::prefetch::IndirectMemory"
    cxx_header = "mem/cache/prefetch/indirect_memory.hh"
    pt_table_entries = Param.MemorySize(
        "16", "Number of entries of the Prefetch Table"
    )
    pt_table_assoc = Param.Unsigned(16, "Associativity of the Prefetch Table")
    pt_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pt_table_assoc,
            size=Parent.pt_table_entries,
        ),
        "Indexing policy of the pattern table",
    )
    pt_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern table"
    )
    max_prefetch_distance = Param.Unsigned(16, "Maximum prefetch distance")
    num_indirect_counter_bits = Param.Unsigned(
        3, "Number of bits of the indirect counter"
    )
    ipd_table_entries = Param.MemorySize(
        "4", "Number of entries of the Indirect Pattern Detector"
    )
    ipd_table_assoc = Param.Unsigned(
        4, "Associativity of the Indirect Pattern Detector"
    )
    ipd_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.ipd_table_assoc,
            size=Parent.ipd_table_entries,
        ),
        "Indexing policy of the Indirect Pattern Detector",
    )
    ipd_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the Indirect Pattern Detector"
    )
    shift_values = VectorParam.Int([2, 3, 4, -3], "Shift values to evaluate")
    addr_array_len = Param.Unsigned(4, "Number of misses tracked")
    prefetch_threshold = Param.Unsigned(
        2, "Counter threshold to start the indirect prefetching"
    )
    stream_counter_threshold = Param.Unsigned(
        4, "Counter threshold to enable the stream prefetcher"
    )
    streaming_distance = Param.Unsigned(
        4, "Number of prefetches to generate when using the stream prefetcher"
    )


class SignaturePathPrefetcher(QueuedPrefetcher):
    type = "SignaturePathPrefetcher"
    cxx_class = "gem5::prefetch::SignaturePath"
    cxx_header = "mem/cache/prefetch/signature_path.hh"

    signature_shift = Param.UInt8(
        3, "Number of bits to shift when calculating a new signature"
    )
    signature_bits = Param.UInt16(12, "Size of the signature, in bits")
    signature_table_entries = Param.MemorySize(
        "1024", "Number of entries of the signature table"
    )
    signature_table_assoc = Param.Unsigned(
        2, "Associativity of the signature table"
    )
    signature_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.signature_table_assoc,
            size=Parent.signature_table_entries,
        ),
        "Indexing policy of the signature table",
    )
    signature_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the signature table"
    )

    num_counter_bits = Param.UInt8(
        3, "Number of bits of the saturating counters"
    )
    pattern_table_entries = Param.MemorySize(
        "4096", "Number of entries of the pattern table"
    )
    pattern_table_assoc = Param.Unsigned(
        1, "Associativity of the pattern table"
    )
    strides_per_pattern_entry = Param.Unsigned(
        4, "Number of strides stored in each pattern entry"
    )
    pattern_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pattern_table_assoc,
            size=Parent.pattern_table_entries,
        ),
        "Indexing policy of the pattern table",
    )
    pattern_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern table"
    )

    prefetch_confidence_threshold = Param.Float(
        0.5, "Minimum confidence to issue prefetches"
    )
    lookahead_confidence_threshold = Param.Float(
        0.75, "Minimum confidence to continue exploring lookahead entries"
    )


class SignaturePathPrefetcherV2(SignaturePathPrefetcher):
    type = "SignaturePathPrefetcherV2"
    cxx_class = "gem5::prefetch::SignaturePathV2"
    cxx_header = "mem/cache/prefetch/signature_path_v2.hh"

    signature_table_entries = "256"
    signature_table_assoc = 1
    pattern_table_entries = "512"
    pattern_table_assoc = 1
    num_counter_bits = 4
    prefetch_confidence_threshold = 0.25
    lookahead_confidence_threshold = 0.25

    global_history_register_entries = Param.MemorySize(
        "8", "Number of entries of global history register"
    )
    global_history_register_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.global_history_register_entries,
            size=Parent.global_history_register_entries,
        ),
        "Indexing policy of the global history register",
    )
    global_history_register_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the global history register"
    )


class AccessMapPatternMatching(ClockedObject):
    type = "AccessMapPatternMatching"
    cxx_class = "gem5::prefetch::AccessMapPatternMatching"
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"

    block_size = Param.Unsigned(
        Parent.block_size,
        "Cacheline size used by the prefetcher using this object",
    )

    limit_stride = Param.Unsigned(
        0, "Limit the strides checked up to -X/X, if 0, disable the limit"
    )
    start_degree = Param.Unsigned(
        4, "Initial degree (Maximum number of prefetches generated"
    )
    hot_zone_size = Param.MemorySize("2KiB", "Memory covered by a hot zone")
    access_map_table_entries = Param.MemorySize(
        "256", "Number of entries in the access map table"
    )
    access_map_table_assoc = Param.Unsigned(
        8, "Associativity of the access map table"
    )
    access_map_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.access_map_table_assoc,
            size=Parent.access_map_table_entries,
        ),
        "Indexing policy of the access map table",
    )
    access_map_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the access map table"
    )
    high_coverage_threshold = Param.Float(
        0.25, "A prefetch coverage factor bigger than this is considered high"
    )
    low_coverage_threshold = Param.Float(
        0.125, "A prefetch coverage factor smaller than this is considered low"
    )
    high_accuracy_threshold = Param.Float(
        0.5, "A prefetch accuracy factor bigger than this is considered high"
    )
    low_accuracy_threshold = Param.Float(
        0.25, "A prefetch accuracy factor smaller than this is considered low"
    )
    high_cache_hit_threshold = Param.Float(
        0.875, "A cache hit ratio bigger than this is considered high"
    )
    low_cache_hit_threshold = Param.Float(
        0.75, "A cache hit ratio smaller than this is considered low"
    )
    epoch_cycles = Param.Cycles(256000, "Cycles in an epoch period")
    offchip_memory_latency = Param.Latency(
        "30ns", "Memory latency used to compute the required memory bandwidth"
    )


class AMPMPrefetcher(QueuedPrefetcher):
    type = "AMPMPrefetcher"
    cxx_class = "gem5::prefetch::AMPM"
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"
    ampm = Param.AccessMapPatternMatching(
        AccessMapPatternMatching(), "Access Map Pattern Matching object"
    )


class DeltaCorrelatingPredictionTables(SimObject):
    type = "DeltaCorrelatingPredictionTables"
    cxx_class = "gem5::prefetch::DeltaCorrelatingPredictionTables"
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    deltas_per_entry = Param.Unsigned(
        20, "Number of deltas stored in each table entry"
    )
    delta_bits = Param.Unsigned(12, "Bits per delta")
    delta_mask_bits = Param.Unsigned(
        8, "Lower bits to mask when comparing deltas"
    )
    table_entries = Param.MemorySize("128", "Number of entries in the table")
    table_assoc = Param.Unsigned(128, "Associativity of the table")
    table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1, assoc=Parent.table_assoc, size=Parent.table_entries
        ),
        "Indexing policy of the table",
    )
    table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the table"
    )


class DCPTPrefetcher(QueuedPrefetcher):
    type = "DCPTPrefetcher"
    cxx_class = "gem5::prefetch::DCPT"
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    dcpt = Param.DeltaCorrelatingPredictionTables(
        DeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object",
    )


class IrregularStreamBufferPrefetcher(QueuedPrefetcher):
    type = "IrregularStreamBufferPrefetcher"
    cxx_class = "gem5::prefetch::IrregularStreamBuffer"
    cxx_header = "mem/cache/prefetch/irregular_stream_buffer.hh"

    num_counter_bits = Param.Unsigned(
        2, "Number of bits of the confidence counter"
    )
    chunk_size = Param.Unsigned(
        256, "Maximum number of addresses in a temporal stream"
    )
    degree = Param.Unsigned(4, "Number of prefetches to generate")
    training_unit_assoc = Param.Unsigned(
        128, "Associativity of the training unit"
    )
    training_unit_entries = Param.MemorySize(
        "128", "Number of entries of the training unit"
    )
    training_unit_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.training_unit_assoc,
            size=Parent.training_unit_entries,
        ),
        "Indexing policy of the training unit",
    )
    training_unit_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the training unit"
    )

    prefetch_candidates_per_entry = Param.Unsigned(
        16, "Number of prefetch candidates stored in a SP-AMC entry"
    )
    address_map_cache_assoc = Param.Unsigned(
        128, "Associativity of the PS/SP AMCs"
    )
    address_map_cache_entries = Param.MemorySize(
        "128", "Number of entries of the PS/SP AMCs"
    )
    ps_address_map_cache_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.address_map_cache_assoc,
            size=Parent.address_map_cache_entries,
        ),
        "Indexing policy of the Physical-to-Structural Address Map Cache",
    )
    ps_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Physical-to-Structural Address Map Cache",
    )
    sp_address_map_cache_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.address_map_cache_assoc,
            size=Parent.address_map_cache_entries,
        ),
        "Indexing policy of the Structural-to-Physical Address Mao Cache",
    )
    sp_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Structural-to-Physical Address Map Cache",
    )


class SlimAccessMapPatternMatching(AccessMapPatternMatching):
    start_degree = 2
    limit_stride = 4


class SlimDeltaCorrelatingPredictionTables(DeltaCorrelatingPredictionTables):
    table_entries = "256"
    table_assoc = 256
    deltas_per_entry = 9


class SlimAMPMPrefetcher(QueuedPrefetcher):
    type = "SlimAMPMPrefetcher"
    cxx_class = "gem5::prefetch::SlimAMPM"
    cxx_header = "mem/cache/prefetch/slim_ampm.hh"

    ampm = Param.AccessMapPatternMatching(
        SlimAccessMapPatternMatching(), "Access Map Pattern Matching object"
    )
    dcpt = Param.DeltaCorrelatingPredictionTables(
        SlimDeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object",
    )


class BOPPrefetcher(QueuedPrefetcher):
    type = "BOPPrefetcher"
    cxx_class = "gem5::prefetch::BOP"
    cxx_header = "mem/cache/prefetch/bop.hh"
    score_max = Param.Unsigned(31, "Max. score to update the best offset")
    round_max = Param.Unsigned(100, "Max. round to update the best offset")
    bad_score = Param.Unsigned(10, "Score at which the HWP is disabled")
    rr_size = Param.Unsigned(64, "Number of entries of each RR bank")
    tag_bits = Param.Unsigned(12, "Bits used to store the tag")
    offset_list_size = Param.Unsigned(
        46, "Number of entries in the offsets list"
    )
    negative_offsets_enable = Param.Bool(
        True,
        "Initialize the offsets list also with negative values \
                (i.e. the table will have half of the entries with positive \
                offsets and the other half with negative ones)",
    )
    delay_queue_enable = Param.Bool(True, "Enable the delay queue")
    delay_queue_size = Param.Unsigned(
        15, "Number of entries in the delay queue"
    )
    delay_queue_cycles = Param.Cycles(
        60,
        "Cycles to delay a write in the left RR table from the delay \
                queue",
    )

    # BOP is a degree one prefetcher
    degree = Param.Int(1, "Number of prefetches to generate")

    queue_squash = True
    queue_filter = True
    cache_snoop = True
    prefetch_on_pf_hit = True
    on_miss = True
    on_inst = False


class SmsPrefetcher(QueuedPrefetcher):
    # Paper: https://web.eecs.umich.edu/~twenisch/papers/isca06.pdf
    type = "SmsPrefetcher"
    cxx_class = "gem5::prefetch::Sms"
    cxx_header = "mem/cache/prefetch/sms.hh"
    ft_size = Param.Unsigned(64, "Size of Filter and Active generation table")
    pht_size = Param.Unsigned(16384, "Size of pattern history table")
    region_size = Param.Unsigned(4096, "Spatial region size")

    queue_squash = True
    queue_filter = True
    cache_snoop = True
    prefetch_on_access = True
    on_inst = False


class SBOOEPrefetcher(QueuedPrefetcher):
    type = "SBOOEPrefetcher"
    cxx_class = "gem5::prefetch::SBOOE"
    cxx_header = "mem/cache/prefetch/sbooe.hh"
    latency_buffer_size = Param.Int(32, "Entries in the latency buffer")
    sequential_prefetchers = Param.Int(9, "Number of sequential prefetchers")
    sandbox_entries = Param.Int(1024, "Size of the address buffer")
    score_threshold_pct = Param.Percent(
        25,
        "Min. threshold to issue a \
        prefetch. The value is the percentage of sandbox entries to use",
    )


class STeMSPrefetcher(QueuedPrefetcher):
    type = "STeMSPrefetcher"
    cxx_class = "gem5::prefetch::STeMS"
    cxx_header = "mem/cache/prefetch/spatio_temporal_memory_streaming.hh"

    spatial_region_size = Param.MemorySize(
        "2KiB", "Memory covered by a hot zone"
    )
    active_generation_table_entries = Param.MemorySize(
        "64", "Number of entries in the active generation table"
    )
    active_generation_table_assoc = Param.Unsigned(
        64, "Associativity of the active generation table"
    )
    active_generation_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.active_generation_table_assoc,
            size=Parent.active_generation_table_entries,
        ),
        "Indexing policy of the active generation table",
    )
    active_generation_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the active generation table"
    )

    pattern_sequence_table_entries = Param.MemorySize(
        "16384", "Number of entries in the pattern sequence table"
    )
    pattern_sequence_table_assoc = Param.Unsigned(
        16384, "Associativity of the pattern sequence table"
    )
    pattern_sequence_table_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1,
            assoc=Parent.pattern_sequence_table_assoc,
            size=Parent.pattern_sequence_table_entries,
        ),
        "Indexing policy of the pattern sequence table",
    )
    pattern_sequence_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern sequence table"
    )

    region_miss_order_buffer_entries = Param.Unsigned(
        131072, "Number of entries of the Region Miss Order Buffer"
    )
    add_duplicate_entries_to_rmob = Param.Bool(
        True, "Add duplicate entries to RMOB"
    )
    reconstruction_entries = Param.Unsigned(
        256, "Number of reconstruction entries"
    )


class HWPProbeEventRetiredInsts(HWPProbeEvent):
    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbeRetiredInsts(
                    self.obj.getCCObject(), name
                )


class PIFPrefetcher(QueuedPrefetcher):
    type = "PIFPrefetcher"
    cxx_class = "gem5::prefetch::PIF"
    cxx_header = "mem/cache/prefetch/pif.hh"
    cxx_exports = [PyBindMethod("addEventProbeRetiredInsts")]

    prec_spatial_region_bits = Param.Unsigned(
        2, "Number of preceding addresses in the spatial region"
    )
    succ_spatial_region_bits = Param.Unsigned(
        8, "Number of subsequent addresses in the spatial region"
    )
    compactor_entries = Param.Unsigned(2, "Entries in the temp. compactor")
    stream_address_buffer_entries = Param.Unsigned(7, "Entries in the SAB")
    history_buffer_size = Param.Unsigned(16, "Entries in the history buffer")

    index_entries = Param.MemorySize("64", "Number of entries in the index")
    index_assoc = Param.Unsigned(64, "Associativity of the index")
    index_indexing_policy = Param.TaggedIndexingPolicy(
        TaggedSetAssociative(
            entry_size=1, assoc=Parent.index_assoc, size=Parent.index_entries
        ),
        "Indexing policy of the index",
    )
    index_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the index"
    )

    def listenFromProbeRetiredInstructions(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        self.addEvent(
            HWPProbeEventRetiredInsts(self, simObj, "RetiredInstsPC")
        )


class FetchDirectedPrefetcher(BasePrefetcher):
    type = "FetchDirectedPrefetcher"
    cxx_class = "gem5::prefetch::FetchDirectedPrefetcher"
    cxx_header = "mem/cache/prefetch/fdp.hh"
    cxx_exports = [PyBindMethod("setCache")]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._cache = None

    def regProbeListeners(self):
        if self._cache:
            self.getCCObject().setCache(self._cache.getCCObject())
        super().regProbeListeners()

    def registerCache(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._cache = simObj

    cpu = Param.BaseCPU(Parent.any, "The CPU to train the predictor")

    latency = Param.Cycles(1, "Latency for generated prefetches")
    pfq_size = Param.Unsigned(64, "Maximum number of queued prefetches")
    tq_size = Param.Unsigned(64, "Maximum number of outstanding translations")

    mark_req_as_prefetch = Param.Bool(
        True,
        "Mark memory requests as prefetches. Allows different handlings of "
        "request. E.g. the Arm TLB drops prefetch requests on a miss.",
    )
    squash_prefetches = Param.Bool(
        True,
        "Squash the prefetch associated with a fetch target in case it gets "
        "removded fron the the FTQ (Fetch consumes it or a pipeline flush).",
    )
    cache_snoop = Param.Bool(
        True,
        "Snoop the icache (if present) and do not enqueue prefetches for "
        "blocks already in the cache.",
    )

class CMCPrefetcher(QueuedPrefetcher):
    type = "CMCPrefetcher"
    cxx_class = "gem5::prefetch::CMCPrefetcher"
    cxx_header = "mem/cache/prefetch/cmc.hh"

    use_virtual_addresses = False
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False
    on_miss = True
    prefetch_on_access = False
    prefetch_on_pf_hit = True  # TODO: check these!
    # cross_pages = True
    cachetags = Param.BaseTags(Parent.tags, "Cache we belong to")
        
    storage_entries = Param.MemorySize(
        "16384",
        "Number of CMC storage entries"
    )
    storage_assoc = Param.Int(8, "Associativity of the CMC storage table")
    degree = Param.Int(16, "Number of prefetches to generate")
    use_last_branch_taken = Param.Bool(
        False,
        "Use the previous execution's saved branch PC for the same load PC "
        "as a small head-delta chooser hint on top of the baseline CMC key",
    )
    prev_branch_chooser_topk = Param.Unsigned(
        2,
        "Number of head-delta candidates to consider from the prev-branch "
        "chooser",
    )
    prev_branch_chooser_use_taken = Param.Bool(
        False,
        "Include the previous branch taken bit in the prev-branch chooser key",
    )
    prev_branch_chooser_min_samples = Param.Unsigned(
        8,
        "Minimum samples before a prev-branch chooser entry can issue hints",
    )
    prev_branch_chooser_min_confidence = Param.Unsigned(
        4,
        "Minimum top-candidate count before a prev-branch chooser entry can "
        "issue hints",
    )
    prev_branch_chooser_min_top_pct = Param.Unsigned(
        60,
        "Minimum percentage of samples belonging to the top head-delta "
        "candidate before a prev-branch chooser entry can issue hints",
    )
    prev_branch_chooser_limit_on_hit = Param.Bool(
        False,
        "When the prev-branch chooser predicts a head delta, limit the issued "
        "prefetches instead of issuing the full baseline CMC stream",
    )
    prev_branch_chooser_modify_baseline = Param.Bool(
        True,
        "Allow the prev-branch chooser to reorder, construct, or limit the "
        "baseline CMC miss/prefetched-hit stream. Disable this when using the "
        "chooser only to train utility and issue separate access-time heads",
    )
    prev_branch_chooser_adaptive_limit = Param.Bool(
        False,
        "Limit the baseline CMC stream only for chooser entries whose online "
        "utility score reaches the throttle threshold",
    )
    prev_branch_chooser_only_predicted = Param.Bool(
        False,
        "When limiting on chooser hit, issue only the predicted head delta "
        "and no baseline fallback candidates",
    )
    prev_branch_chooser_baseline_fallback_degree = Param.Unsigned(
        2,
        "Number of baseline CMC candidates to keep after the predicted head "
        "delta when limiting on chooser hit",
    )
    prev_branch_chooser_construct_predicted = Param.Bool(
        True,
        "When limiting on chooser hit, construct the predicted head address "
        "from the predicted delta if it is not already in the baseline stream",
    )
    prev_branch_chooser_selective_construct = Param.Bool(
        False,
        "When not limiting on chooser hit, construct a missing predicted head "
        "address and issue it before the unchanged baseline CMC stream",
    )
    prev_branch_chooser_construct_min_samples = Param.Unsigned(
        0,
        "Additional minimum samples required before constructing a missing "
        "predicted head; 0 reuses the normal chooser eligibility threshold",
    )
    prev_branch_chooser_construct_min_confidence = Param.Unsigned(
        0,
        "Additional minimum candidate count required before constructing a "
        "missing predicted head; 0 reuses the normal chooser threshold",
    )
    prev_branch_chooser_construct_min_top_pct = Param.Unsigned(
        0,
        "Additional minimum percentage of samples for the specific predicted "
        "delta before constructing a missing head; 0 disables this extra check",
    )
    prev_branch_chooser_construct_cache_filter = Param.Bool(
        True,
        "Skip constructed predicted heads that are already in cache or MSHR",
    )
    prev_branch_chooser_use_utility_score = Param.Bool(
        False,
        "Use online same-load-PC next-delta feedback to gate constructed "
        "heads and throttling decisions",
    )
    prev_branch_chooser_construct_min_score = Param.Int(
        2,
        "Minimum utility score required before constructing a missing "
        "chooser-predicted head",
    )
    prev_branch_chooser_throttle_min_score = Param.Int(
        4,
        "Minimum utility score required before limiting the baseline CMC "
        "stream on a chooser hit",
    )
    prev_branch_chooser_utility_max_score = Param.Int(
        31,
        "Saturation magnitude for per-candidate chooser utility scores",
    )
    prev_branch_access_predictor = Param.Bool(
        False,
        "Allow high-utility previous-branch chooser entries to issue one "
        "predicted head on ordinary data accesses, while leaving baseline CMC "
        "lookup/training on miss or prefetched-hit events only",
    )
    prev_branch_access_min_score = Param.Int(
        2,
        "Minimum utility score required for the access-time branch-head "
        "predictor to issue",
    )
    prev_branch_access_lookahead = Param.Unsigned(
        1,
        "Maximum multiples of the predicted delta to try for the access-time "
        "branch-head predictor. Values above 1 allow it to skip an already "
        "cached immediate head and issue one farther-ahead address",
    )
    prev_branch_access_cache_filter = Param.Bool(
        True,
        "Skip access-time branch-head predictions already in cache or MSHR",
    )
    prev_branch_filter_biased = Param.Bool(
        False,
        "Ignore previous-branch chooser hints from branch PCs whose outcomes "
        "are almost always one-sided",
    )
    prev_branch_bias_min_samples = Param.Unsigned(
        64,
        "Minimum samples before a previous branch PC can be classified as "
        "biased",
    )
    prev_branch_bias_max_pct = Param.Unsigned(
        98,
        "Classify a previous branch PC as biased if its dominant outcome "
        "reaches this percentage",
    )
    prev_branch_dump_file = Param.String(
        "",
        "Optional CSV path for dumping prev-branch/next-address samples",
    )
    prev_branch_dump_limit = Param.Unsigned(
        0,
        "Maximum number of prev-branch analysis samples to dump",
    )
    storage_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.storage_assoc,
            size=Parent.storage_entries),
        "Indexing policy of active generation table"
    )
    storage_replacement_policy = Param.BaseReplacementPolicy(
        BRRIPRP(),
        "Replacement policy of active generation table"
    )

add_citation(
    FetchDirectedPrefetcher,
    """@inproceedings{10.1145/3613424.3614258,
  author    = {Schall, David and
               Sandberg, Andreas and
               Grot, Boris},
  title     = {Warming Up a Cold Front-End with Ignite},
  year      = {2023},
  publisher = {Association for Computing Machinery},
  address   = {Toronto, ON, Canada},
  doi       = {10.1145/3613424.3614258},
  booktitle = {Proceedings of the 56th Annual IEEE/ACM International Symposium on Microarchitecture (MICRO '23)},
  series    = {MICRO'23}
}
""",
)
