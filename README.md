#streaming_lag#

streaming_lag is an experimental extension to measure the lag of a
streaming slave in units of time instead of bytes.

The idea is to create a table with exactly one row and one column
which is a timestamp. A background process writes this timestamp
on a regular basis, like every 1sec. Now, given the master and the
slave are time-synchronized via NTP or similar, the difference of
clock_timestamp() minus that value on the slave returns a good
measure how far it lags behind the master in units of time.

As it is my first extension, this module is also a learning exercise.
I have tested it only on Linux.

##Compile and install it##

To build it, just do this:

```
make && sudo make install
```

You need GNU `make` and `pg_config` which comes with Postgres. If
you are using a Postgres package provided by the OS vendor, you
probably want to install a development package. If `pg_config` is
not found in your `$PATH`, point the `PG_CONFIG` environment
variable to it:

```
     PG_CONFIG=/path/to/pg_config make -e &&
sudo PG_CONFIG=/path/to/pg_config make -e install
```
##Create and configure the extension##

The next step is to create the extension and to configure it. Since
streaming replication works on the whole cluster, the extension is
installed in exactly one database of the cluster. By default this
is `postgres`. So, log in to the database as superuser:

```
psql postgres postgres
```

and create the extension:

```
CREATE EXTENSION streaming_lag;
```

This command creates 2 objects, a table called `streaming_lag_data` and a
view called `streaming_lag`. By default they are created in the
`public` schema. However, you can create them in a different schema:

```
CREATE EXTENSION streaming_lag SCHEMA lag;
```

You can also move the objects later to a different schema using:

```
ALTER EXTENSION streaming_lag SET SCHEMA my_lag;
```

Once the extension is created, you need to add a few values to your
`postgresql.conf`:

```
shared_preload_libraries = 'streaming_lag'
streaming_lag.database = 'postgres'
streaming_lag.schema = 'public'
streaming_lag.precision = 5000
```

If you already have set `shared_preload_libraries`, append `streaming_lag`
to it separated by a comma, e.g.

```
shared_preload_libraries = 'pg_stat_statements,streaming_lag'
```

Next, restart the database. You should see the following lines in the
log file:

```
LOG:  registering background worker "streaming_lag"
LOG:  loaded library "streaming_lag"
LOG:  starting background worker process "streaming_lag"
LOG:  streaming_lag: initialized, database objects validated
```

###Configuration variables###

* `streaming_lag.database`
specifies the database where the extension was created.
To change the value postgres has to be restarted.

* `streaming_lag.schema`
the schema
To change the value postgres has to be restarted.

* `streaming_lag.precision`
specifies the frequency at which the timestamp is written in
milliseconds. By default a timestamp is written every 5000ms.
This value can be changed in SIGHUP context. That means a full
restart is not required. Just change the value in the
`postgresql.conf` and reload the database:

```
sudo -u postgres pg_ctl reload -D /path/to/data/directory
```

If `streaming_lag.precision` is set to `0`, the table is not
updated anymore. You can use that to temporary disable the
feature. In that case the lag reported on the slave will be
growing with time.

##Usage##

Provided both, master and slave, have synchronized clocks, you
can measure the lag on the slave by:

```
postgres=# select * from lag.streaming_lag;
       lag       
-----------------
 00:00:04.647359
(1 row)
```
