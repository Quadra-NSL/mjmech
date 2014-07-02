#!/usr/bin/python

# Copyright 2014 Josh Pieper, jjp@pobox.com.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

'''A simple command line tool for commanding and monitoring Dongbu
HerkuleX servos.'''

import trollius as asyncio
from trollius import From, Return
import optparse

from legtool.servo import herkulex

def get_address(options):
    if options.address is None:
        return herkulex.HerkuleX.BROADCAST

    return int(options.address)

@asyncio.coroutine
def do_enumerate(servo, options):
    print(yield From(servo.enumerate()))

@asyncio.coroutine
def do_set_address(servo, options):
    address = int(options.set_address)
    if address == servo.BROADCAST:
        raise RuntimeError('address cannot be set using broadcast address')
    yield From(
        servo.eep_write(get_address(options), servo.EEP_ID, [ address ]))

@asyncio.coroutine
def do_reboot(servo, options):
    yield From(servo.reboot(get_address(options)))

@asyncio.coroutine
def do_status(servo, options):
    print(yield From(servo.status(get_address(options))))

@asyncio.coroutine
def do_voltage(servo, options):
    print(yield From(servo.voltage(get_address(options))))

@asyncio.coroutine
def do_temperature(servo, options):
    print(yield From(servo.temperature_C(get_address(options))))

def main():
    parser = optparse.OptionParser()
    parser.add_option('-a', '--address', default=None,
                      help='servo to communicate with')
    parser.add_option('-d', '--device', default='/dev/ttyUSB0',
                      help='serial port device')
    parser.add_option('-e', '--enumerate', action='store_true', default=None,
                      help='enumerate servos on the bus')
    parser.add_option('--set-address', dest='set_address', default=None,
                      help='set all servos on the bus to the given address')
    parser.add_option('-r', '--reboot', action='store_true', default=None,
                      help='reboot servos')
    parser.add_option('-s', '--status', action='store_true', default=None,
                      help='query status of servo')
    parser.add_option('-v', '--voltage', action='store_true', default=None,
                      help='query voltage of servo')
    parser.add_option('-t', '--temperature', action='store_true', default=None,
                      help='query temperature of servo')

    (options, args) = parser.parse_args()

    actions = {
        'enumerate': do_enumerate,
        'set_address': do_set_address,
        'reboot': do_reboot,
        'status': do_status,
        'voltage': do_voltage,
        'temperature': do_temperature,
        }

    action_func = None
    for key in actions.keys():
        if hasattr(options, key) and getattr(options, key) is not None:
            if action_func is not None:
                raise RuntimeError('more than one action set')
            action_func = actions[key]

    servo = herkulex.HerkuleX(options.device)

    loop = asyncio.get_event_loop()
    if action_func is not None:
        loop.run_until_complete(action_func(servo, options))
        return
    else:
        raise RuntimeError('no action specified')

if __name__ == '__main__':
    main()
