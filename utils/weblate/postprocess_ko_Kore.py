
import sys
import yaml

replacements = [
    [r'\ ', ''],
    [r'\...', '…'],
    [r'\..', '‥'],
    [r'\.', '。'],
    [r'\,', '、'],
    [r'\?', '？'],
    [r'\!', '！'],
    [r'\:', '：'],
    [r'\;', '；'],
]

def postprocess(input_file, output_file):
    with open(input_file, 'r', encoding='utf-8') as f:
        data = yaml.safe_load(f)
    result = {}
    for replacement in replacements:
        result['weblate'] = [(value.replace(replacement[0], replacement[1])) for value in data['weblate']]
    with open(output_file, 'w', encoding='utf-8') as f:
        yaml.safe_dump(data, f, allow_unicode=True)

def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print('Usage: python3 %s <input yaml> <output yaml>' % sys.argv[0])
        sys.exit(1)
    input_file, output_file = args
    postprocess(input_file, output_file)

if __name__ == '__main__':
    main()
