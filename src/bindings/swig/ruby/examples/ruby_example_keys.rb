
# include libelektra 'kdb' module
require 'kdb'


# create a new empty key
k = Kdb::Key.new

# create a new key with initail name
k2 = Kdb::Key.new "user/myapp/#1/config1"

# create a new fully initialized key
k3 = Kdb::Key.new("user/myapp/#1/config1",
                  value: "some value",
                  meta_data: "important info",
                  owner: "me")

# create a new Key with special flags
k4 = Kdb::Key.new("user/myapp/#1/bconfig",
                  flags: Kdb::KEY_BINARY)



# set a name
begin
        k.name= "user/myapp/#1/config1"
rescue Kdb::KeyInvalidName
        puts "invalid key name given"
end

# get name methods
puts "k.name:      #{k.name}"
puts "k.base_name: #{k.base_name}"
puts "k.full_name: #{k.full_name}"
puts "k.namespace: #{k.namespace}"

# name manipulations
begin
        puts "k.add_name"
        k.add_name "../config2"
        puts "k.name: #{k.name}"

        puts "k.add_base_name"
        k.add_base_name "width"
        puts "k.name #{k.name}"
rescue Kdb::KeyInvalidName
        puts "invalid key name given"
end
     
# set a value
k.value= "120 px"

# get value
puts "k.value: #{k.value}"


#
# working with binary keys
#

# create an initially binary key
kbin = Kdb::Key.new("user/myapp/#1/binkey", flags: Kdb::KEY_BINARY)
# can be tested 
puts "kbin.is_binary?: #{kbin.is_binary?}"

# use the same value methods
kbin.value = "\000\001\002\003" 
v = kbin.value
puts "kbin value: #{v.unpack("H*").first}"

# you can also use the get_binary, however be careful, calling this method
# on a non-binary key will throw a Kdb::KeyTypeMismatch exception
v = kbin.get_binary


# if key is not initially binary
kbin2 = Kdb::Key.new "user/myapp/#1/binkey2"

# use the set_binary method to set a binary value. After this call
# the key will be a binary key
kbin2.set_binary "\000\001\002"

kbin2.is_binary? # => true



#
# working with metadata
#

# create a new key with initially set meta data
kmeta = Kdb::Key.new("uset/myapp/config/#1/c1",
                     owner: "me",   # meta data
                     # no meta, will set the value of the key
                     value: "some value",
                     # meta data
                     created: "some years ago",
                     importance: "high",
                     some_thing_stupid: "...")

# adding meta data to a Key
kmeta.set_meta "another meta", "value"
# there's also an Array-like access method
kmeta["jet another"] = "jet value"

# accessing meta data
mv = kmeta.get_meta "another meta"
mv = kmeta["jet another"]

# check if meta data exists
kmeta.has_meta? "jet another"  # => true


#
# metadata iterator
#

# kdb style
kmeta.rewind_meta  # reset internal meta data cursor

while not kmeta.next_meta.nil? do
        # kmeat.next_meta advances the internal cursor and returns
        # the next meta data key

        mk = kmeta.current_meta

        # we get a Kdb::Key which holds the metadata values
        mk.name
        mk.value
end

# ruby style iteration
kmeta.meta.each do |mk|
        puts "kmeta metadata: #{mk.name} => #{mk.value}"
end

long_names_metadata = kmeta.meta.find_all { |mk| mk.name.size >= 8 }
# here we get a Ruby Array containing meta data keys, which meet the above
# criteria
long_names_metadata.each do |mk|
        puts "kmeta long name metadata: #{mk.name} => #{mk.value}"
end


