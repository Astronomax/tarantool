local t = require('luatest')
local server = require('luatest.server')

-- gh-12284: The build must catch duplicate keys in existing data and during
-- concurrent inserts.
local g_dup = t.group('index_build.dup_key',
                       t.helpers.matrix{engine = {'memtx', 'vinyl'}})

g_dup.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({box_cfg = {memtx_use_mvcc_engine = true}})
    cg.server:start()
end)

g_dup.after_all(function(cg)
    cg.server:drop()
end)

g_dup.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

-- Build of a unique secondary index fails when existing data has duplicates.
g_dup.test_dup_in_existing_data = function(cg)
    cg.server:exec(function(engine)
        local s = box.schema.space.create('test', {engine = engine})
        s:create_index('pk')
        s:insert{1, 10}
        s:insert{2, 10}  -- same sk value as {1,10}

        t.assert_error_covers({type = 'ClientError', code = box.error.TUPLE_FOUND},
                               s.create_index, s, 'sk', {parts = {{2, 'unsigned'}}})
        -- Space must still be usable after the failed build.
        t.assert_equals(s:select{}, {{1, 10}, {2, 10}})
    end, {cg.params.engine})
end

-- A concurrent insert of a duplicate secondary key is rejected while the
-- build is in progress.
g_dup.test_concurrent_dup_rejected = function(cg)
    cg.server:exec(function(engine)
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = engine})
        s:create_index('pk')
        s:insert{1, 10}
        s:insert{2, 20}

        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', true)
        local f = fiber.new(function()
            return s:create_index('sk', {parts = {{2, 'unsigned'}}})
        end)
        f:set_joinable(true)

        -- Wait until the first build batch commits: _index_local receives a
        -- cursor row. insert_main for {1,10} runs before that commit, so once
        -- the row appears get_prepared(sk=10) is guaranteed to succeed.
        for _ = 1, 100 do
            fiber.sleep(0.001)
            if #box.space._index_local:select{} > 0 then break end
        end
        t.assert(#box.space._index_local:select{} > 0,
                 'first build batch not committed')

        -- {1,10} is in the building index; inserting {3,10} duplicates sk=10.
        t.assert_error_covers({type = 'ClientError', code = box.error.TUPLE_FOUND},
                               s.insert, s, {3, 10})
        -- Non-conflicting insert must succeed.
        s:insert{3, 30}

        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        local ok, err = f:join(10)
        t.assert(ok, tostring(err))
        t.assert_equals(box.space.test.index.sk:select{},
                        {{1, 10}, {2, 20}, {3, 30}})
    end, {cg.params.engine})
end

-- gh-10170: Concurrent reads and writes to the space must not be aborted
-- by the memtx MVCC index build (the old code called space_invalidate which
-- conflicted all concurrent readers).
local g_no_abort = t.group('index_build.concurrent_not_aborted')

g_no_abort.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({box_cfg = {memtx_use_mvcc_engine = true}})
    cg.server:start()
end)

g_no_abort.after_all(function(cg)
    cg.server:drop()
end)

g_no_abort.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

g_no_abort.test_reads_not_aborted = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local log = require('log')
        log.info('TEST: creating space')
        local s = box.schema.space.create('test', {engine = 'memtx'})
        log.info('TEST: creating pk')
        s:create_index('pk')
        log.info('TEST: inserting 20 tuples')
        for i = 1, 20 do
            s:insert{i, i * 10}
        end
        log.info('TEST: setting errinj')
        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', true)
        log.info('TEST: creating build fiber')
        local f = fiber.new(function()
            log.info('BUILD FIBER: starting create_index sk')
            local res = s:create_index('sk', {unique = false,
                                          parts = {{2, 'unsigned'}}})
            log.info('BUILD FIBER: create_index sk done')
            return res
        end)
        f:set_joinable(true)
        log.info('TEST: yielding to let build start')

        -- Wait until the first build batch commits.
        local waited = 0
        for _ = 1, 100 do
            fiber.sleep(0.001)
            waited = waited + 1
            if #box.space._index_local:select{} > 0 then break end
        end
        log.info(string.format('TEST: waited %d iters, _index_local rows: %d',
                            waited, #box.space._index_local:select{}))
        t.assert(#box.space._index_local:select{} > 0,
                 'first build batch not committed')

        -- A concurrent read transaction must not be conflicted by the build.
        log.info('TEST: starting read txn')
        box.begin()
        local tuple = s:get(1)
        t.assert_not_equals(tuple, nil)
        log.info('TEST: committing read txn')
        local ok, err = pcall(box.commit)
        t.assert(ok, tostring(err))
        log.info('TEST: read txn committed ok')

        -- Concurrent inserts of keys the build hasn't visited yet must succeed.
        for i = 21, 25 do
            log.info(string.format('TEST: inserting {%d, %d}', i, i * 10))
            local ins_ok, ins_err = pcall(s.insert, s, {i, i * 10})
            t.assert(ins_ok, tostring(ins_err))
        end

        log.info('TEST: clearing errinj')
        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        log.info('TEST: joining build fiber')
        local build_ok, build_err = f:join(10)
        t.assert(build_ok, tostring(build_err))
        log.info('TEST: build done, checking result')
        t.assert_equals(#box.space.test.index.sk:select{}, 25)
        log.info('TEST: PASS')
    end)
end

-- After a build with concurrent inserts, the secondary index must contain
-- exactly the same tuples as the primary index.
local g_correct = t.group('index_build.correctness',
                           t.helpers.matrix{engine = {'memtx', 'vinyl'}})

g_correct.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({box_cfg = {memtx_use_mvcc_engine = true}})
    cg.server:start()
end)

g_correct.after_all(function(cg)
    cg.server:drop()
end)

g_correct.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

g_correct.test_sk_matches_pk = function(cg)
    cg.server:exec(function(engine)
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = engine})
        s:create_index('pk')
        for i = 1, 30 do
            s:insert{i, i * 10}
        end

        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', true)
        local f = fiber.new(function()
            return s:create_index('sk', {unique = false,
                                          parts = {{2, 'unsigned'}}})
        end)
        f:set_joinable(true)
        -- Yield to let the build register its trigger.
        for _ = 1, 20 do fiber.sleep(0) end

        -- Insert additional tuples while build is running.
        for i = 31, 35 do
            s:insert{i, i * 10}
        end

        box.error.injection.set('ERRINJ_BUILD_INDEX_DELAY', false)
        local ok, err = f:join(10)
        t.assert(ok, tostring(err))

        t.assert_equals(s.index.sk:select{}, s.index.pk:select{})
    end, {cg.params.engine})
end

-- A functional index must be built correctly over pre-existing data.
local g_func = t.group('index_build.functional')

g_func.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)

g_func.after_all(function(cg)
    cg.server:drop()
end)

g_func.after_each(function(cg)
    cg.server:exec(function()
        if box.space.test ~= nil then
            box.space.test:drop()
        end
        if box.func.kf ~= nil then
            box.func.kf:drop()
        end
    end)
end)

g_func.test_func_index_build = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test')
        s:create_index('pk')
        for i = 1, 5 do
            s:insert{i, tostring(i * 10)}
        end

        box.schema.func.create('kf', {
            is_sandboxed = true,
            is_deterministic = true,
            body = 'function(t) return {t[2]} end',
        })
        local idx = s:create_index('fi', {
            func = 'kf',
            parts = {{1, 'string'}},
        })
        t.assert_equals(idx:select{'10'}, {{1, '10'}})
        t.assert_equals(idx:select{'50'}, {{5, '50'}})
        t.assert_equals(#idx:select{}, 5)
    end)
end

-- A multikey (JSON-path [*]) index must be built correctly over pre-existing
-- data.
local g_mk = t.group('index_build.multikey')

g_mk.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)

g_mk.after_all(function(cg)
    cg.server:drop()
end)

g_mk.after_each(function(cg)
    cg.server:exec(function()
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

g_mk.test_multikey_index_build = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test')
        s:create_index('pk')
        s:insert{1, {10, 20, 30}}
        s:insert{2, {40}}
        s:insert{3, {10, 50}}

        local idx = s:create_index('mk', {
            unique = false,
            parts = {{2, 'unsigned', path = '[*]'}},
        })
        t.assert_equals(idx:select{10}, {{1, {10, 20, 30}}, {3, {10, 50}}})
        t.assert_equals(idx:select{20}, {{1, {10, 20, 30}}})
        t.assert_equals(idx:select{40}, {{2, {40}}})
        t.assert_equals(idx:select{50}, {{3, {10, 50}}})
    end)
end
