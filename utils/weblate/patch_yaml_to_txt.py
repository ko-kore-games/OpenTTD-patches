
import sys
import yaml
import os.path as path

header = """
##name Korean (Mixed Script)
##ownname 韓國語
##isocode ko_Kore
##plural 11
##textdir ltr
##digitsep ,
##digitsepcur ,
##decimalsep .
##winlangid 0x0c12
##grflangid 0x3b
##gender m f
##case case1
"""

def get_key(line):
    key = line.split(':')[0]
    return key

def convert_line(line, updated):
    if line == '' or line[0] == '#':
        return line

    key = get_key(line)
    stripped = key.strip()

    if stripped in updated:
        return '%s:%s' % (key, updated[stripped])
    else:
        return line

def convert(base, updated):
    lines = base.split('\n')
    lines = [convert_line(line, updated['weblate']) for line in lines]
    if lines[0].startswith('##'):
        lines = lines[12:]
        result = header.strip() + '\n' + '\n'.join(lines)
    else:
        result = '\n'.join(lines)
    return result

def main():
    if len(sys.argv) < 3:
        print('Usage: python3 %s <base txt file> <updated yaml file>' % sys.argv[0])
        sys.exit(1)

    base_file = sys.argv[1]
    if not path.exists(base_file):
        print('File %s does not exist' % base_file)
        sys.exit(1)

    updated_file = sys.argv[2]
    if not path.exists(updated_file):
        print('File %s does not exist' % updated_file)
        sys.exit(1)

    with open(base_file, 'r') as f:
        base = f.read()
    with open(updated_file, 'r') as f:
        updated = yaml.load(f, Loader=yaml.FullLoader)

    print(convert(base, updated))

if __name__ == '__main__':
    main()
