local t = require('luatest')
local server = require('luatest.server')

local g = t.group()

g.before_each(function(cg)
    cg.server = server:new({box_cfg = {
        vinyl_cache = 0, vinyl_page_index_cache = 0, vinyl_page_info_cache = 0,
        vinyl_page_index_btree_in_memory_depth_max = 0,
        vinyl_page_index_btree_fanout = 2, vinyl_page_info_block_size = 3,
    }})
    cg.server:start()
end)

g.after_each(function(cg)
    cg.server:drop()
end)

g.test_leaf_boundaries = function(cg)
    cg.server:exec(function()
        for _, engine in ipairs({'memtx', 'vinyl'}) do
            local s = box.schema.space.create(engine, {engine = engine})
            local opts = {parts = {{1, 'unsigned'}, {2, 'unsigned'}}}
            if engine == 'vinyl' then opts.page_size = 128 end
            s:create_index('pk', opts)
            for a = 1, 8 do
                for b = 1, 17 do
                    s:insert({a * 2, b * 2, string.rep('z', 130)})
                end
            end
        end
        box.snapshot()
    end)
    -- Node sizes and fanout for existing runs come from the file.
    cg.server:restart({box_cfg = {
        vinyl_cache = 0, vinyl_page_index_cache = 0, vinyl_page_info_cache = 0,
        vinyl_page_index_btree_in_memory_depth_max = 0,
        vinyl_page_index_btree_fanout = 128, vinyl_page_info_block_size = 64,
    }})
    cg.server:exec(function()
        local fiber = require('fiber').self()
        local s = box.space.vinyl
        local reads = box.stat.vinyl().page_index.io.read_ops
        local csw = fiber:csw()
        local q = s:quantile(0.5, {4}, {14})
        t.assert_equals(fiber:csw(), csw)
        t.assert_equals(box.stat.vinyl().page_index.io.read_ops, reads)
        t.assert(q[1] >= 4 and q[1] < 14)
        for _, key in ipairs({{}, {0}, {2}, {8}, {9}, {16}, {18},
                              {2, 2}, {4, 16}, {8, 17}, {16, 34}}) do
            for _, typ in ipairs({'EQ', 'REQ', 'GE', 'GT', 'LE', 'LT'}) do
                local opts = {iterator = typ}
                t.assert_equals(s:select(key, opts),
                                box.space.memtx:select(key, opts))
            end
        end
        local pi = box.stat.vinyl().page_index
        t.assert_equals(pi.key_cache.mem_used, 0)
        t.assert_equals(pi.page_info_cache.mem_used, 0)
        t.assert_gt(pi.io.read_ops, reads)
    end)
end

g.test_leaf_cache_without_parent = function(cg)
    cg.server:restart({box_cfg = {
        vinyl_cache = 0, vinyl_page_index_cache = 0, vinyl_page_info_cache = 800,
        vinyl_page_index_btree_in_memory_depth_max = 0,
        vinyl_page_index_btree_fanout = 2, vinyl_page_info_block_size = 16,
    }})
    cg.server:exec(function()
        local s = box.schema.space.create('s', {engine = 'vinyl'})
        s:create_index('pk', {page_size = 128})
        for i = 1, 256 do s:insert({i, string.rep('x', 160)}) end
        box.snapshot()
        local function pi() return box.stat.vinyl().page_index end
        local start = pi().io.read_ops
        t.assert_equals(s:get(6)[1], 6)
        local cold = pi().io.read_ops
        t.assert_gt(cold, start)
        for i = 3, 7 do t.assert_equals(s:get(i)[1], i) end
        t.assert_equals(pi().io.read_ops, cold)
        local fiber = require('fiber')
        local done = fiber.channel(8)
        for i = 1, 8 do
            fiber.create(function()
                local ok, err = pcall(function()
                    for j = 1, 64 do
                        local key = (j * 17 + i) % 256 + 1
                        t.assert_equals(s:get(key)[1], key)
                    end
                end)
                done:put(ok or tostring(err))
            end)
        end
        for _ = 1, 8 do t.assert_equals(done:get(), true) end
        t.assert_equals(pi().key_cache.mem_used, 0)
        t.assert_gt(pi().page_info_cache.hit, 0)
        t.assert_gt(pi().page_info_cache.evict, 0)
        t.assert_le(pi().page_info_cache.mem_used, 800)
        s:drop()
        t.assert_equals(pi().page_info_cache.mem_used, 0)
    end)
end

g.test_compressed_wide_leaf = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('s', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'string'}}, page_size = 128})
        for i = 1, 3 do
            s:insert({string.rep('k', 1024) .. i, string.rep('z', 70000)})
        end
        box.snapshot()
    end)
    cg.server:restart()
    cg.server:exec(function()
        local fio = require('fio')
        local mp = require('msgpack')
        local s = box.space.s
        local path = fio.glob(fio.pathjoin(box.cfg.vinyl_dir, s.id,
                                           0, '*.index'))[1]
        local manifest
        for _, row in require('xlog').pairs(path) do
            if row.HEADER.type == 'BTREE' and type(row.BODY[1]) == 'table' then
                manifest = row.BODY[1]
            end
        end
        local ref = tostring(manifest[6])
        local offset = tonumber((mp.decode('\xcf' .. ref:sub(1, 8))))
        local size = mp.decode('\xce' .. ref:sub(9, 12))
        local unpacked = mp.decode('\xce' .. ref:sub(13, 16))
        t.assert_lt(size, unpacked)
        local function get(i) return s:get(string.rep('k', 1024) .. i) end
        local io = box.stat.vinyl().page_index.io
        t.assert_equals(#get(1)[2], 70000)
        local after = box.stat.vinyl().page_index.io
        t.assert_equals(after.read_ops - io.read_ops, 1)
        t.assert_equals(after.read_bytes - io.read_bytes, size)
        local fd = assert(fio.open(path, {'O_RDWR'}))
        local pos = offset + size - 1
        local byte = assert(fd:pread(1, pos))
        assert(fd:pwrite(string.char(255 - byte:byte()), pos))
        t.assert_error_msg_contains('checksum', get, 3)
        fd:close()
    end)
    cg.server:restart({box_cfg = {force_recovery = true, vinyl_cache = 0,
        vinyl_page_index_btree_in_memory_depth_max = 0}})
    cg.server:exec(function()
        for i = 1, 3 do
            t.assert_equals(#box.space.s:get(string.rep('k', 1024) .. i)[2], 70000)
        end
    end)
end
