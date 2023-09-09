
import sys
import re
import yaml

replacements = [
    [' ', ''],
    ['...', '…'],
    ['..', '‥'],
    ['.', '。'],
    [',', '、'],
    ['?', '？'],
    ['!', '！'],
    [':', '：'],
    [';', '；'],
]

def replace_value(value):
    brackets = []
    def strip(m):
        idx = len(brackets)
        brackets.append(m.group(0))
        return "${%d}" % idx
    value = re.sub(r'\{([^\n}]+)\}', strip, value)
    for replacement in replacements:
        value = value.replace(replacement[0], replacement[1])
    def restore(m):
        return brackets[int(m.group(1))]
    value = re.sub(r'\$\{(\d+)\}', restore, value)
    return value

def postprocess(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as f:
        data = yaml.safe_load(f)
    result = {}
    result['weblate'] = {key: replace_value(value) for key, value in data['weblate'].items()}
    with open(output_file, 'w', encoding='utf-8') as f:
        yaml.safe_dump(result, f, allow_unicode=True)

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print('Usage: python3 %s <input yaml> <output yaml>' % sys.argv[0])
        sys.exit(1)
    input_file, output_file = args
    postprocess(input_file, output_file)

if __name__ == '__main__':
    main()
