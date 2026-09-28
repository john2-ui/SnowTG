set pagination off
set confirm off
set breakpoint pending on
break owner_timer_engine_fini
commands
silent
printf "DRAIN_COUNTER timer owner=%u active=%u\n", engine->lcore_id, engine->active
if engine->active != 0
  quit 3
end
continue
end
break tg_flow_pool_fini
commands
silent
printf "DRAIN_COUNTER flow capacity=%u free=%u\n", pool->capacity, pool->free_count
if pool->free_count != pool->capacity
  quit 4
end
continue
end
run
