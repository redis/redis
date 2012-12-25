##
# Time ISO Test

if Object.const_defined?(:Time)
  assert('Time.new', '15.2.3.3.3') do
    Time.new.class == Time
  end

  assert('Time', '15.2.19') do
    Time.class == Class
  end

  assert('Time superclass', '15.2.19.2') do
    Time.superclass == Object
  end

  assert('Time.at', '15.2.19.6.1') do
    Time.at(1300000000.0)
  end

  assert('Time.gm', '15.2.19.6.2') do
    Time.gm(2012, 12, 23)
  end

  assert('Time.local', '15.2.19.6.3') do
    Time.local(2012, 12, 23)
  end

  assert('Time.mktime', '15.2.19.6.4') do
    Time.mktime(2012, 12, 23)
  end

  assert('Time.now', '15.2.19.6.5') do
    Time.now.class == Time
  end

  assert('Time.utc', '15.2.19.6.6') do
    Time.utc(2012, 12, 23)
  end

  assert('Time#+', '15.2.19.7.1') do
    t1 = Time.at(1300000000.0)
    t2 = t1.+(60)

    t2.utc.asctime == "Sun Mar 13 07:07:40 UTC 2011"
  end

  assert('Time#-', '15.2.19.7.2') do
    t1 = Time.at(1300000000.0)
    t2 = t1.-(60)

    t2.utc.asctime == "Sun Mar 13 07:05:40 UTC 2011"
  end

  assert('Time#<=>', '15.2.19.7.3') do
    t1 = Time.at(1300000000.0)
    t2 = Time.at(1400000000.0)
    t3 = Time.at(1500000000.0)

    t2.<=>(t1) == 1 and
      t2.<=>(t2) == 0 and
      t2.<=>(t3) == -1 and
      t2.<=>(nil) == nil
  end

  assert('Time#asctime', '15.2.19.7.4') do
    Time.at(1300000000.0).utc.asctime == "Sun Mar 13 07:06:40 UTC 2011"
  end

  assert('Time#ctime', '15.2.19.7.5') do
    Time.at(1300000000.0).utc.ctime == "Sun Mar 13 07:06:40 UTC 2011"
  end

  assert('Time#day', '15.2.19.7.6') do
    Time.gm(2012, 12, 23).day == 23
  end

  assert('Time#dst?', '15.2.19.7.7') do
    not Time.gm(2012, 12, 23).utc.dst?
  end

  assert('Time#getgm', '15.2.19.7.8') do
    Time.at(1300000000.0).getgm.asctime == "Sun Mar 13 07:06:40 UTC 2011"
  end

  assert('Time#getlocal', '15.2.19.7.9') do
    t1 = Time.at(1300000000.0)
    t2 = Time.at(1300000000.0)
    t3 = t1.getlocal

    t1 == t3 and t3 == t2.getlocal
  end

  assert('Time#getutc', '15.2.19.7.10') do
    Time.at(1300000000.0).getutc.asctime == "Sun Mar 13 07:06:40 UTC 2011"
  end

  assert('Time#gmt?', '15.2.19.7.11') do
    Time.at(1300000000.0).utc.gmt?
  end

  # ATM not implemented
  # assert('Time#gmt_offset', '15.2.19.7.12') do

  assert('Time#gmtime', '15.2.19.7.13') do
    Time.at(1300000000.0).gmtime
  end

  # ATM not implemented
  # assert('Time#gmtoff', '15.2.19.7.14') do

  assert('Time#hour', '15.2.19.7.15') do
    Time.gm(2012, 12, 23, 7, 6).hour == 7
  end

  # ATM doesn't really work
  # assert('Time#initialize', '15.2.19.7.16') do

  assert('Time#initialize_copy', '15.2.19.7.17') do
    time_tmp_2 = Time.at(7.0e6)
    time_tmp_2.clone == time_tmp_2
  end

  assert('Time#localtime', '15.2.19.7.18') do
    t1 = Time.at(1300000000.0)
    t2 = Time.at(1300000000.0)

    t1.localtime
    t1 == t2.getlocal
  end

  assert('Time#mday', '15.2.19.7.19') do
    Time.gm(2012, 12, 23).mday == 23
  end

  assert('Time#min', '15.2.19.7.20') do
    Time.gm(2012, 12, 23, 7, 6).min == 6
  end

  assert('Time#mon', '15.2.19.7.21') do
    Time.gm(2012, 12, 23).mon == 12
  end

  assert('Time#month', '15.2.19.7.22') do
    Time.gm(2012, 12, 23).month == 12
  end

  assert('Times#sec', '15.2.19.7.23') do
    Time.gm(2012, 12, 23, 7, 6, 40).sec == 40
  end

  assert('Time#to_f', '15.2.19.7.24') do
    Time.at(1300000000.0).to_f == 1300000000.0
  end

  assert('Time#to_i', '15.2.19.7.25') do
    Time.at(1300000000.0).to_i == 1300000000
  end

  assert('Time#usec', '15.2.19.7.26') do
    Time.at(1300000000.0).usec == 0
  end

  assert('Time#utc', '15.2.19.7.27') do
    Time.at(1300000000.0).utc
  end

  assert('Time#utc?', '15.2.19.7.28') do
    Time.at(1300000000.0).utc.utc?
  end

  # ATM not implemented
  # assert('Time#utc_offset', '15.2.19.7.29') do

  assert('Time#wday', '15.2.19.7.30') do
    Time.gm(2012, 12, 23).wday == 0
  end

  assert('Time#yday', '15.2.19.7.31') do
    Time.gm(2012, 12, 23).yday == 358
  end

  assert('Time#year', '15.2.19.7.32') do
    Time.gm(2012, 12, 23).year == 2012
  end

  assert('Time#zone', '15.2.19.7.33') do
    Time.at(1300000000.0).utc.zone == 'UTC'
  end

  # Not ISO specified

  assert('Time#to_s') do
    Time.at(1300000000.0).utc.to_s == "Sun Mar 13 07:06:40 UTC 2011"
  end

  assert('Time#inspect') do
    Time.at(1300000000.0).utc.inspect == "Sun Mar 13 07:06:40 UTC 2011"
  end
end

