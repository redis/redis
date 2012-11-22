##
# Comparable
#
# ISO 15.3.3
module Comparable

  ##
  # Return true if +self+ is less
  # than +other+. Otherwise return
  # false.
  #
  # ISO 15.3.3.2.1
  def < other
    cmp = self <=> other
    if cmp.nil?
      false
    elsif cmp < 0
      true
    else
      false
    end
  end

  ##
  # Return true if +self+ is less
  # than or equal to +other+.
  # Otherwise return false.
  # 
  # ISO 15.3.3.2.2
  def <= other
    cmp = self <=> other
    if cmp.nil?
      false
    elsif cmp <= 0
      true
    else
      false
    end
  end

  ##
  # Return true if +self+ is equal
  # to +other+. Otherwise return
  # false.
  #
  # ISO 15.3.3.2.3
  def == other
    cmp = self <=> other
    if cmp == 0
      true
    else
      false
    end
  end

  ##
  # Return true if +self+ is greater
  # than +other+. Otherwise return
  # false.
  #
  # ISO 15.3.3.2.4
  def > other
    cmp = self <=> other
    if cmp.nil?
      false
    elsif cmp > 0
      true
    else
      false
    end
  end

  ##
  # Return true if +self+ is greater
  # than or equal to +other+.
  # Otherwise return false.
  #
  # ISO 15.3.3.2.5
  def >= other
    cmp = self <=> other
    if cmp.nil?
      false
    elsif cmp >= 0
      true
    else
      false
    end
  end

  ##
  # Return true if +self+ is greater 
  # than or equal to +min+ and
  # less than or equal to +max+.
  # Otherwise return false.
  #
  # ISO 15.3.3.2.6
  def between?(min, max)
    if self < min or self > max
      false
    else
      true
    end
  end
end
